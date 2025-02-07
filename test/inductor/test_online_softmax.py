# Owner(s): ["module: inductor"]

import math
import os

from triton.testing import do_bench

import torch
import torch._inductor.config as inductor_config
from torch._dynamo.utils import same
from torch._inductor.test_case import run_tests, TestCase
from torch._inductor.utils import run_and_get_code
from torch.testing._internal.common_utils import (
    instantiate_parametrized_tests,
    IS_LINUX,
    parametrize,
)
from torch.testing._internal.inductor_utils import GPU_TYPE, HAS_CUDA


DO_PERF_TEST = os.environ.get("DO_PERF_TEST") == "1"


class TestOnlineSoftmax(TestCase):
    def do_test_acc_and_perf(self, op):
        N = 32 * 1024
        V = 50304  # padded version for gpt2

        def f(x):
            return op(x, dim=-1)

        x = torch.randn(N, V, dtype=torch.bfloat16, device=GPU_TYPE)
        opt_f = torch.compile(f)
        expected = f(x)
        actual = opt_f(x)

        self.assertTrue(torch.allclose(expected, actual, atol=1e-2, rtol=1e-2))

        if DO_PERF_TEST:
            eager_ms = do_bench(lambda: f(x))
            opt_ms = do_bench(lambda: opt_f(x))
            print(f"{eager_ms=}")
            print(f"{opt_ms=}")

    def test_softmax(self):
        self.do_test_acc_and_perf(torch.softmax)

    def test_log_softmax(self):
        self.do_test_acc_and_perf(torch.log_softmax)

    def get_softmax_wrapper(self, V=50304, use_log_softmax=False, device=GPU_TYPE):
        N = 32 * 1024

        @torch.compile
        def f(x):
            if use_log_softmax:
                return torch.log_softmax(x, dim=-1)
            else:
                return torch.softmax(x, dim=-1)

        x = torch.randn(N, V, dtype=torch.bfloat16, device=device)
        out, source_codes = run_and_get_code(f, x)
        return source_codes[0]

    def test_codegen_3pass_softmax_due_to_disable(self):
        with inductor_config.patch(online_softmax=False):
            wrapper_code = self.get_softmax_wrapper()

        self.assertEqual(wrapper_code.count("for r0_offset in"), 3)

    @parametrize("V", [2048, 50304])
    @parametrize("use_log_softmax", [False, True])
    def test_codegen_online_softmax(self, use_log_softmax, V):
        wrapper_code = self.get_softmax_wrapper(use_log_softmax=use_log_softmax, V=V)

        self.assertEqual(wrapper_code.count("for r0_offset in"), 2)

    def test_no_online_softmax_for_cpu(self):
        code = self.get_softmax_wrapper(V=2048, device="cpu")

        # CPU need an explicit loop across different rows.
        # For GPU, this is parallelized by the hardware.
        self.assertEqual(code.count("for(int64_t"), 4)

    def test_codegen_softmax_persistent_reduction(self):
        """
        Persistent reduction has no for loops.
        """
        wrapper_code = self.get_softmax_wrapper(1024)
        self.assertEqual(wrapper_code.count("for r0_offset in"), 0)

    @inductor_config.patch("triton.persistent_reductions", False)
    def test_sdpa(self):
        """
        Make sure online softmax here does not conflict with the sdpa
        patterns.
        """
        q, k, v = (
            torch.randn((4, 2, 16, 32), device=GPU_TYPE, dtype=torch.bfloat16)
            for _ in range(3)
        )

        def f(q, k, v):
            return (
                torch.matmul(q, k.transpose(-2, -1))
                .div(math.sqrt(k.shape[-1]))
                .softmax(dim=-1)
                .matmul(v)
            )

        opt_f = torch.compile(f)
        ref = f(q, k, v)
        act, (code,) = run_and_get_code(opt_f, q, k, v)
        self.assertTrue(torch.allclose(ref, act, atol=1e-2, rtol=1e-2))
        self.assertTrue("aten._scaled_dot_product_" in code)

    @parametrize("nrow", [2, 2048])
    @parametrize("dim", [-1, 0, 1])
    def test_prepare_softmax(self, dim, nrow):
        def f(x, dim):
            xmax = x.amax(dim=dim, keepdim=True)
            xsum = (x - xmax).exp().sum(dim=dim, keepdim=True)
            return xmax, xsum

        x = torch.randn(nrow, 2048, dtype=torch.bfloat16, device=GPU_TYPE)
        act, (code,) = run_and_get_code(torch.compile(f), x, dim)
        ref = f(x, dim)
        self.assertTrue(same(ref, act, tol=1e-2))

        if nrow == 2048 and dim == 0:
            # split reduction is triggered. We have multiple kernels
            self.assertTrue(code.count("def triton") >= 2)
        else:
            if nrow == 2 and dim == 0:
                # persistent reduction triggered
                expected_num_loop = 0
            else:
                # A single loop due to online softmax
                expected_num_loop = 1
            self.assertEqual(code.count("for r0_offset in"), expected_num_loop)

    def test_split_reduction(self):
        """
        We don't split online_softmax_reduce for now. Check
        'Split online_softmax_reduce' note in the code.

        When a split is promsing, we fallback for now.

        This is just a manual example rather than something we
        see in practice.
        """
        # tensor shape to trigger split reduction
        x = torch.randn(1, 2**20, dtype=torch.bfloat16, device=GPU_TYPE)
        ref = torch.softmax(x, dim=-1)
        act, (code,) = run_and_get_code(torch.compile(torch.softmax), x, dim=-1)
        self.assertTrue(torch.allclose(ref, act, atol=1e-3, rtol=1e-3))
        self.assertTrue(code.count("def triton") >= 2)
        self.assertTrue("online_softmax_reduce" not in code)


instantiate_parametrized_tests(TestOnlineSoftmax)

if __name__ == "__main__":
    if IS_LINUX and HAS_CUDA:
        run_tests()
