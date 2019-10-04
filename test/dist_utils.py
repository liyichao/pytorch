from __future__ import absolute_import, division, print_function, unicode_literals

from collections import namedtuple
from functools import wraps
from os import getenv

import torch.distributed as dist
from torch.distributed.rpc_api import RpcBackend


if not dist.is_available():
    print("c10d not available, skipping tests")
    sys.exit(0)


TestConfig = namedtuple("TestEnv", ["backend"])


TEST_CONFIG = TestConfig(backend=getenv("RPC_BACKEND", RpcBackend.PROCESS_GROUP))
INIT_METHOD_TEMPLATE = "file://{file_name}?rank={rank}&world_size={world_size}"


def dist_init(test_method):
    """
    We use this decorator for setting up and tearing down state since
    MultiProcessTestCase runs each `test*` method in a separate process and
    each process just runs the `test*` method without actually calling
    'setUp' and 'tearDown' methods of unittest.
    """

    @wraps(test_method)
    def wrapper(self, *arg, **kwargs):
        self.worker_id = self.rank
        dist.init_process_group(backend="gloo", init_method=self.init_method)
        dist.init_model_parallel(
            self_name="worker%d" % self.rank,
            backend=TEST_CONFIG.backend,
            self_rank=self.rank,
            init_method=self.init_method,
        )
        test_method(self, *arg, **kwargs)
        dist.join_rpc()

    return wrapper
