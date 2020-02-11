import oneflow as flow
import numpy as np

def test_negative(test_case):
    func_config = flow.FunctionConfig()
    func_config.default_data_type(flow.float)
    func_config.default_distribute_strategy(flow.distribute.consistent_strategy())

    @flow.function(func_config)
    def NegativeJob(a=flow.FixedTensorDef((8,))):
        return flow.math.negative(a)

    x = np.random.uniform(low=-10.0, high=10.0, size=(8,)).astype(np.float32)
    y = NegativeJob(x).get().ndarray()
    test_case.assertTrue(np.allclose(y, -x, equal_nan=True))

