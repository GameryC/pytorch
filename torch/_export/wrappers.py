# mypy: allow-untyped-defs
from contextlib import contextmanager
from typing import Set

import torch
import torch._custom_ops
from torch._C import DispatchKey
from torch._higher_order_ops.strict_mode import strict_mode
from torch._higher_order_ops.utils import autograd_not_implemented
from torch._ops import HigherOrderOperator
from torch._subclasses.fake_tensor import FakeTensorMode
from torch.fx.experimental.proxy_tensor import ProxyTorchDispatchMode, track_tensor_tree
from torch.utils import _pytree as pytree

from torch.fx.experimental.proxy_tensor import (
    get_proxy_slot,
    PreDispatchTorchFunctionMode,
    track_tensor_tree,
)

from torch.utils._python_dispatch import is_traceable_wrapper_subclass_type
from torch._higher_order_ops.flat_apply import to_graphable, flat_apply, ConstantFunction


class ExportTracepoint(HigherOrderOperator):
    def __init__(self):
        super().__init__("_export_tracepoint")

    def __call__(self, *args, **kwargs):
        return super().__call__(*args, **kwargs)


_export_tracepoint = ExportTracepoint()


@_export_tracepoint.py_impl(ProxyTorchDispatchMode)
def export_tracepoint_dispatch_mode(mode, *args, **kwargs):
    p_args, p_kwargs = pytree.tree_map(mode.tracer.unwrap_proxy, (args, kwargs))
    proxy = mode.tracer.create_proxy(
        "call_function", _export_tracepoint, p_args, p_kwargs
    )
    return track_tensor_tree(args, proxy, constant=None, tracer=mode.tracer)


@_export_tracepoint.py_impl(FakeTensorMode)
def export_tracepoint_fake_tensor_mode(mode, *args, **kwargs):
    with mode:
        return args


@_export_tracepoint.py_functionalize_impl
def export_tracepoint_functional(ctx, *args, **kwargs):
    unwrapped_args = ctx.unwrap_tensors(args)
    unwrapped_kwargs = ctx.unwrap_tensors(kwargs)

    with ctx.redispatch_to_next():
        out = _export_tracepoint(*unwrapped_args, **unwrapped_kwargs)
        return ctx.wrap_tensors(out)


_export_tracepoint.py_impl(DispatchKey.Autograd)(
    autograd_not_implemented(_export_tracepoint, deferred_error=True)
)


@_export_tracepoint.py_impl(DispatchKey.CPU)
def export_tracepoint_cpu(*args, **kwargs):
    return args


def _wrap_submodule(mod, path, module_call_specs):
    assert isinstance(mod, torch.nn.Module)
    assert path != ""
    submodule = torch.fx.graph_module._get_attr(mod, path)

    def update_module_call_signatures(path, in_spec, out_spec):
        if path in module_call_specs:
            assert module_call_specs[path]["in_spec"] == in_spec
            assert module_call_specs[path]["out_spec"] == out_spec
        module_call_specs[path] = {"in_spec": in_spec, "out_spec": out_spec}

    def check_flattened(flat_args):
        for a in flat_args:
            if not (isinstance(a, (torch.Tensor, str, int, float, bool)) or a is None):
                raise AssertionError(
                    f"Only Tensors or scalars are supported as pytree flattened inputs, got: {a}"
                )

    def pre_hook(module, args, kwargs):
        flat_args, in_spec = pytree.tree_flatten((args, kwargs))
        check_flattened(flat_args)
        flat_args = _export_tracepoint(*flat_args, kind="module_call_inputs", path=path)
        args, kwargs = pytree.tree_unflatten(flat_args, in_spec)
        return args, kwargs

    def post_hook(module, args, kwargs, res):
        _, in_spec = pytree.tree_flatten((args, kwargs))
        flat_res, out_spec = pytree.tree_flatten(res)
        check_flattened(flat_res)
        flat_res = _export_tracepoint(*flat_res, kind="module_call_outputs", path=path)
        update_module_call_signatures(path, in_spec, out_spec)
        return pytree.tree_unflatten(flat_res, out_spec)

    pre_handle = submodule.register_forward_pre_hook(pre_hook, with_kwargs=True)
    post_handle = submodule.register_forward_hook(post_hook, with_kwargs=True)
    return pre_handle, post_handle


@contextmanager
def _wrap_submodules(f, preserve_signature, module_call_signatures):
    handles = []

    try:
        for path in preserve_signature:
            handles.extend(_wrap_submodule(f, path, module_call_signatures))
        yield
    finally:
        for handle in handles:
            handle.remove()


def _mark_strict_experimental(cls):
    def call(self, *args):
        return strict_mode(self, args)

    cls.__call__ = call
    return cls


def _mark_constructor_exportable_experimental(constructor_subclass):
    def wrapper(*args, **kwargs):
        # TODO container type input is not supported, will need to apply flat_apply
        assert is_traceable_wrapper_subclass_type(type(args[0]))
        constructor_subclass(*args, **kwargs)
        if torch._C._is_torch_function_mode_enabled():
            torch_function_mode_stack = torch.overrides._get_current_function_mode_stack()
            for mode in torch_function_mode_stack:
                if isinstance(mode, PreDispatchTorchFunctionMode):
                    tracer = mode.tracer
                    flat_proxy_args = []
                    subclass = args[0]
                    
                    flat_args, in_spec = to_graphable((tuple(args[1:]), kwargs))

                    qualname = tracer.get_fresh_qualname(constructor_subclass.__name__)
                    setattr(tracer.root, qualname, in_spec)
                    spec_proxy = tracer.create_proxy("get_attr", qualname, (), {})

                    for arg in flat_args:
                        if isinstance(arg, torch.Tensor):
                            proxy = get_proxy_slot(arg, tracer).proxy
                            flat_proxy_args.append(proxy) 
                        else:
                            flat_proxy_args.append(arg)

                    _, func_spec = torch.utils._pytree.tree_flatten(ConstantFunction(type(subclass)))
                    qualname = tracer.get_fresh_qualname(type(subclass).__name__)
                    setattr(tracer.root, qualname, func_spec)
                    func_spec_proxy = tracer.create_proxy("get_attr", qualname, (), {})

                    inner_proxy = tracer.create_proxy(
                        "call_function",
                        flat_apply,
                        (func_spec_proxy, spec_proxy, *flat_proxy_args),
                        {},
                    )
                    track_tensor_tree(
                        args[0], inner_proxy, constant=None, tracer=tracer
                    )
        #return out
    return wrapper
