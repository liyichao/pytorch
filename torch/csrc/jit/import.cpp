#include <ATen/core/functional.h>
#include <c10/util/Exception.h>
#include <torch/csrc/jit/import.h>
#include <torch/csrc/jit/import_export_helpers.h>
#ifndef C10_MOBILE
#include <torch/csrc/jit/import_legacy.h>
#endif
#include <torch/csrc/jit/import_source.h>
#include <torch/csrc/jit/ir.h>
#include <torch/csrc/jit/pickle.h>
#include <torch/csrc/jit/unpickler.h>
#include <torch/csrc/jit/script/script_type_parser.h>
#include <torch/csrc/jit/source_range_serialization.h>

#include "caffe2/serialize/file_adapter.h"
#include "caffe2/serialize/inline_container.h"
#include "caffe2/serialize/istream_adapter.h"

#include <ATen/ATen.h>

#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace torch {
namespace jit {

using caffe2::serialize::FileAdapter;
using caffe2::serialize::IStreamAdapter;
using caffe2::serialize::PyTorchStreamReader;
using caffe2::serialize::ReadAdapterInterface;

void postSetStateValidate(const IValue& v) {
  auto obj = v.toObject();
  const auto& objType = obj->type();
  for (size_t i = 0; i < objType->numAttributes(); i++) {
    const auto& attrType = objType->getAttribute(i);
    const auto& attrName = objType->getAttributeName(i);
    const auto& slot = obj->getSlot(i);
    // const auto attrType = objType->getAttribute(i);
    // Verify that all the non-optional attributes have been initialized
    // TODO: Issue #20497
    if (attrType->kind() != TypeKind::OptionalType) {
      TORCH_CHECK(
          !slot.isNone(),
          "The field '",
          attrName,
          "' was left unitialized after __setstate__, but expected a ",
          "value of type '",
          attrType->python_str(),
          "'");
    }
  }
}

namespace {


// This is a deserializer class which loads script modules from pt files.
// Content of the file is written using PyTorchStreamWriter, for details please
// check caffe2/serialize/inline_container.h.
// The module is saved in pickle. readArchive() is called to parse and construct
// the constant table and the script module.
class ScriptModuleDeserializer final {
 public:
  ScriptModuleDeserializer(
      std::shared_ptr<script::CompilationUnit> cu,
      std::unique_ptr<PyTorchStreamReader> reader)
      : compilation_unit_(cu),
        reader_(std::move(reader)),
        source_importer_(
            compilation_unit_,
            &constants_table_,
            [this](const std::string& qualifier) {
              return findSourceInArchiveFromQualifier(
                  *reader_, export_prefix_, qualifier);
            }) {}

  script::Module deserialize(
      c10::optional<at::Device> device,
      script::ExtraFilesMap& extra_files);

 private:
  IValue readArchive(const std::string& archive_name);

  std::shared_ptr<script::CompilationUnit> compilation_unit_;
  std::unique_ptr<PyTorchStreamReader> reader_;
  c10::optional<at::Device> device_;
  std::vector<at::Tensor> constants_table_;
  script::SourceImporter source_importer_;
  std::string export_prefix_ = "code/";
};

IValue ScriptModuleDeserializer::readArchive(const std::string& archive_name) {
  std::stringstream picklename;
  picklename << archive_name << ".pkl";
  at::DataPtr pickle_ptr;
  size_t pickle_size;
  std::tie(pickle_ptr, pickle_size) = reader_->getRecord(picklename.str());

  size_t bytes_read = 0;
  auto data = reinterpret_cast<const char*>(pickle_ptr.get());
  auto reader = [&](char* buffer, size_t len) -> size_t {
    if (bytes_read >= pickle_size) {
      return 0;
    }
    len = std::min(pickle_size - bytes_read, len);
    // Copy len bytes into buffer
    const char* start = data + bytes_read;
    std::memcpy(buffer, start, len);
    bytes_read += len;
    return len;
  };

  auto class_resolver = [&](const c10::QualifiedName& qn) {
    auto cls = source_importer_.loadNamedType(qn)->expect<ClassType>();
    return c10::StrongTypePtr(compilation_unit_, std::move(cls));
  };

  // Decouple how to get obj from type. In this file it's dependent on
  // Method.run() and graph executor, etc.
  // For bytecode import we need to decouple these dependencies.
  auto obj_loader = [&](at::StrongTypePtr type, IValue input) {
    auto cls = type.type_->expect<at::ClassType>();
    size_t n = cls->numAttributes();
    if (checkHasValidSetGetState(type.type_)) {
      auto obj = c10::ivalue::Object::create(type, n);
      // XXX: Do not optimize __setstate__, so that we don't try to
      // specialize the class before it is initialized.
      setGraphExecutorOptimize(false);
      Function* set_state = type.type_->getMethod("__setstate__");
      // since we are in the middle of unpickling we might still have lists and
      // dicts that do not have accurate tags (e.g. they report they are
      // List[Any]). But we need to run __setstate__ which will check the input
      // type and may access the tags. Since setstate has a known input type, we
      // can correctly restore the tags now by apply the input type of set_state
      // to the state object being passed.
      restoreAccurateTypeTags(
          input, set_state->getSchema().arguments().at(1).type());
      (*set_state)({obj, input});
      setGraphExecutorOptimize(true);
      postSetStateValidate(obj);
      return obj;
    } else {
      auto dict = std::move(input).toGenericDict();
      auto obj = c10::ivalue::Object::create(type, n);
      for (size_t i = 0; i < n; ++i) {
        obj->setSlot(i, dict.at(cls->getAttributeName(i)));
      }
      return obj;
    }
  };

  auto read_record = [&](const std::string& name) {
    std::stringstream ss;
    ss << archive_name << "/" << name;
    return std::get<0>(reader_->getRecord(ss.str()));
  };

  Unpickler unpickler(
      reader, std::move(class_resolver), std::move(obj_loader),
      std::move(read_record), device_);
  return unpickler.parse_ivalue();
}

script::Module ScriptModuleDeserializer::deserialize(
    c10::optional<at::Device> device,
    script::ExtraFilesMap& extra_files) {
  C10_LOG_API_USAGE_ONCE("torch.script.load");
  device_ = device;
  // Load extra files.
  for (const auto& kv : extra_files) {
    const std::string& key = "extra/" + kv.first;
    if (reader_->hasRecord(key)) {
      at::DataPtr meta_ptr;
      size_t meta_size;
      std::tie(meta_ptr, meta_size) = reader_->getRecord(key);
      extra_files[kv.first] =
          std::string(static_cast<char*>(meta_ptr.get()), meta_size);
    }
  }
  if (reader_->hasRecord("model.json")) {
#ifndef C10_MOBILE
    return torch::jit::LEGACY_deserialize(
        compilation_unit_, std::move(reader_), device_);
#else
    AT_ERROR("Legacy model format is not supported on mobile.");
#endif
  }
  auto tuple = readArchive("constants").toTuple();
  for (auto constant : tuple->elements()) {
    constants_table_.push_back(constant.toTensor());
  }
  return script::Module(readArchive("data").toObject());
}

} // namespace

script::Module import_ir_module(
    std::shared_ptr<script::CompilationUnit> cu,
    std::istream& in,
    c10::optional<at::Device> device,
    script::ExtraFilesMap& extra_files) {
  auto reader = torch::make_unique<PyTorchStreamReader>(&in);
  ScriptModuleDeserializer deserializer(std::move(cu), std::move(reader));
  return deserializer.deserialize(device, extra_files);
}

script::Module import_ir_module(
    std::shared_ptr<script::CompilationUnit> cu,
    const std::string& filename,
    c10::optional<at::Device> device,
    script::ExtraFilesMap& extra_files) {
  auto reader = torch::make_unique<PyTorchStreamReader>(filename);
  ScriptModuleDeserializer deserializer(std::move(cu), std::move(reader));
  return deserializer.deserialize(device, extra_files);
}

script::Module import_ir_module(
    std::shared_ptr<script::CompilationUnit> cu,
    std::unique_ptr<ReadAdapterInterface> rai,
    c10::optional<at::Device> device,
    script::ExtraFilesMap& extra_files) {
  auto reader = torch::make_unique<PyTorchStreamReader>(std::move(rai));
  ScriptModuleDeserializer deserializer(std::move(cu), std::move(reader));
  return deserializer.deserialize(device, extra_files);
}

script::Module load(
    std::istream& in,
    c10::optional<at::Device> device,
    script::ExtraFilesMap& extra_files) {
  std::unique_ptr<IStreamAdapter> rai =
      caffe2::make_unique<IStreamAdapter>(&in);
  auto module = load(std::move(rai), device, extra_files);
  return module;
}

script::Module load(
    const std::string& filename,
    c10::optional<at::Device> device,
    script::ExtraFilesMap& extra_files) {
  std::unique_ptr<FileAdapter> rai = caffe2::make_unique<FileAdapter>(filename);
  auto module = load(std::move(rai), device, extra_files);
  return module;
}

script::Module load(
    std::unique_ptr<ReadAdapterInterface> rai,
    c10::optional<c10::Device> device,
    script::ExtraFilesMap& extra_files) {
  auto reader = torch::make_unique<PyTorchStreamReader>(std::move(rai));
  auto cu = std::make_shared<script::CompilationUnit>();
  ScriptModuleDeserializer deserializer(std::move(cu), std::move(reader));
  return deserializer.deserialize(device, extra_files);
}

} // namespace jit
} // namespace torch
