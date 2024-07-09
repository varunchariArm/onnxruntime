# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation.  All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------

"""
Benchmark performance of MultiHeadAttention with Nvidia GPU of Compute Capability 8.0, 8.6 or 8.9 in Linux:
sh benchmark_mha.sh
"""

import csv
import math
import os
import platform
import statistics
import time
from datetime import datetime
from typing import Dict, List, Optional, Tuple

import torch
from onnx import TensorProto, helper
from torch.nn.attention import SDPBackend, sdpa_kernel
from torch.nn.functional import scaled_dot_product_attention

from onnxruntime import InferenceSession, SessionOptions, get_available_providers
from onnxruntime.transformers.io_binding_helper import CudaSession


class InputFormats:
    Q_K_V_BSNH_BSNH_BSNH = 0
    QKV_BSN3H = 1
    Q_KV_BSNH_BSN2H = 2
    Q_K_V_BSNH_BNSH_BNSH = 3  # For cross attention

    @staticmethod
    def input_format_str(format: int) -> str:
        names = InputFormats.get_name_list()
        return names[format]

    @staticmethod
    def convert(format_str: str) -> int:
        names = InputFormats.get_name_list()
        return names.index(format_str)

    @staticmethod
    def get_name_list() -> List[str]:
        return ["Q,K,V", "QKV", "Q,KV", "Q,K',V'"]


class MultiHeadAttentionConfig:
    def __init__(
        self,
        batch_size: int,
        sequence_length: int,
        num_heads: int,
        head_size: int,
        causal: bool,
        past_sequence_length: int = 0,
        kv_sequence_length=None,
        max_cache_sequence_length=None,
        softmax_scale: float = 0.0,
        provider="CPUExecutionProvider",
        device: Optional[torch.device] = None,
        enable_cuda_graph: bool = False,
        dtype=torch.float,
        use_kv_cache: bool = False,
        share_past_present_buffer: bool = False,
        input_format: int = InputFormats.Q_K_V_BSNH_BSNH_BSNH,
    ):
        self.operator = "MultiHeadAttention"
        self.batch_size = batch_size
        self.sequence_length = sequence_length
        self.kv_sequence_length = kv_sequence_length or sequence_length
        self.max_cache_sequence_length = max_cache_sequence_length
        self.past_sequence_length = past_sequence_length
        self.num_heads = num_heads
        self.head_size = head_size
        self.causal = causal
        self.softmax_scale = softmax_scale or (1.0 / (head_size**0.5))

        self.use_kv_cache = use_kv_cache
        if not use_kv_cache:
            assert past_sequence_length == 0
        else:
            assert self.kv_sequence_length == self.sequence_length

        if input_format == InputFormats.Q_K_V_BSNH_BNSH_BNSH:
            # cross attention does not have past state
            assert not use_kv_cache

        # Derived values
        self.total_sequence_length = self.kv_sequence_length + past_sequence_length
        self.past_buffer_length = self.max_cache_sequence_length if share_past_present_buffer else past_sequence_length
        self.present_buffer_length = (
            self.max_cache_sequence_length if share_past_present_buffer else self.total_sequence_length
        )

        self.provider = provider
        self.device = device
        self.enable_cuda_graph = enable_cuda_graph
        self.dtype = dtype

        self.share_past_present_buffer = share_past_present_buffer
        self.input_format = input_format
        self.is_packed_qkv = input_format == InputFormats.QKV_BSN3H
        self.is_packed_kv = input_format == InputFormats.Q_KV_BSNH_BSN2H

    def __repr__(self):
        return (
            f"MultiHeadAttentionConfig(batch_size={self.batch_size}, sequence_length={self.sequence_length}, "
            f"num_heads={self.num_heads}, head_size={self.head_size}, "
            f"kv_sequence_length={self.kv_sequence_length}, past_sequence_length={self.past_sequence_length}, "
            f"max_cache_sequence_length={self.max_cache_sequence_length},"
            f"causal={self.causal}), softmax_scale={self.softmax_scale}, use_kv_cache={self.use_kv_cache}, "
            f"share_past_present_buffer={self.share_past_present_buffer}, "
            f"provider={self.provider}, device={self.device}, enable_cuda_graph={self.enable_cuda_graph}, "
            f"dtype={self.dtype}, input_format={InputFormats.input_format_str(self.input_format)}"
        )

    def shape_dict(self, input_format=None):
        shapes: Dict[str, Tuple] = {
            "output": (self.batch_size, self.sequence_length, self.num_heads * self.head_size),
        }

        input_format = input_format or self.input_format
        if input_format == InputFormats.QKV_BSN3H:
            shapes = {
                **shapes,
                "query": (self.batch_size, self.sequence_length, self.num_heads, 3, self.head_size),
            }
        elif input_format == InputFormats.Q_KV_BSNH_BSN2H:
            shapes = {
                **shapes,
                "query": (self.batch_size, self.sequence_length, self.num_heads * self.head_size),
                "key": (self.batch_size, self.sequence_length, self.num_heads, 2, self.head_size),
            }
        elif input_format == InputFormats.Q_K_V_BSNH_BSNH_BSNH:
            shapes = {
                **shapes,
                "query": (self.batch_size, self.sequence_length, self.num_heads * self.head_size),
                "key": (self.batch_size, self.sequence_length, self.num_heads * self.head_size),
                "value": (self.batch_size, self.sequence_length, self.num_heads * self.head_size),
            }
        else:
            assert input_format == InputFormats.Q_K_V_BSNH_BNSH_BNSH
            shapes = {
                **shapes,
                "query": (self.batch_size, self.sequence_length, self.num_heads * self.head_size),
                "key": (self.batch_size, self.num_heads, self.sequence_length, self.head_size),
                "value": (self.batch_size, self.num_heads, self.sequence_length, self.head_size),
            }

        if self.use_kv_cache:
            assert input_format != InputFormats.Q_K_V_BSNH_BNSH_BNSH, "cross attention shall not have past state"
            shapes = {
                **shapes,
                "past_key": (self.batch_size, self.num_heads, self.past_buffer_length, self.head_size),
                "past_value": (self.batch_size, self.num_heads, self.past_buffer_length, self.head_size),
                "present_key": (self.batch_size, self.num_heads, self.present_buffer_length, self.head_size),
                "present_value": (self.batch_size, self.num_heads, self.present_buffer_length, self.head_size),
            }

        return shapes

    def random_inputs(self, seed: int = 123):
        device = self.device
        dtype = self.dtype

        shape_dict = self.shape_dict()

        if seed > 0:
            torch.manual_seed(seed)

        shape = (self.batch_size, self.sequence_length, self.num_heads, self.head_size)
        q = torch.empty(shape, device=device, dtype=dtype).normal_(mean=0, std=0.1)
        k = torch.empty(shape, device=device, dtype=dtype).normal_(mean=0, std=0.1)
        v = torch.empty(shape, device=device, dtype=dtype).normal_(mean=0, std=0.1)
        k_bnsh = k.transpose(1, 2)
        v_bnsh = v.transpose(1, 2)

        if self.input_format == InputFormats.Q_K_V_BSNH_BSNH_BSNH:
            feeds = {
                "query": q.reshape(shape_dict["query"]),
                "key": k.reshape(shape_dict["key"]),
                "value": v.reshape(shape_dict["value"]),
            }
        elif self.input_format == InputFormats.QKV_BSN3H:
            query = q.view(self.batch_size * self.sequence_length, self.num_heads, self.head_size)
            key = k.view(self.batch_size * self.sequence_length, self.num_heads, self.head_size)
            value = v.view(self.batch_size * self.sequence_length, self.num_heads, self.head_size)
            feeds = {
                "query": torch.dstack((query, key, value)).reshape(shape_dict["query"]).contiguous(),
            }
        elif self.input_format == InputFormats.Q_KV_BSNH_BSN2H:
            key = k.view(self.batch_size * self.sequence_length, self.num_heads, self.head_size)
            value = v.view(self.batch_size * self.sequence_length, self.num_heads, self.head_size)
            feeds = {
                "query": q.reshape(shape_dict["query"]),
                "key": torch.dstack((key, value)).reshape(shape_dict["key"]).contiguous(),
            }
        else:
            assert self.input_format == InputFormats.Q_K_V_BSNH_BNSH_BNSH
            feeds = {
                "query": q.reshape(shape_dict["query"]),
                "key": k_bnsh.contiguous(),
                "value": v_bnsh.contiguous(),
            }

        if self.use_kv_cache:
            feeds = {
                **feeds,
                "past_key": torch.empty(shape_dict["past_key"], device=device, dtype=dtype).normal_(mean=0, std=0.1),
                "past_value": torch.empty(shape_dict["past_value"], device=device, dtype=dtype).normal_(
                    mean=0, std=0.1
                ),
            }

        return feeds

    def get_input_output_names(self):
        if self.input_format == InputFormats.Q_K_V_BSNH_BNSH_BNSH:
            return ["query", "key"], ["output"]

        if self.input_format == InputFormats.QKV_BSN3H:
            inputs, outputs = ["query"], ["output"]
        elif self.input_format == InputFormats.Q_KV_BSNH_BSN2H:
            inputs, outputs = ["query", "key"], ["output"]
        else:
            inputs, outputs = ["query", "key", "value"], ["output"]

        if self.use_kv_cache:
            return [*inputs, "past_key", "past_value"], [*outputs, "present_key", "present_value"]
        else:
            return inputs, outputs


def fill_optional_mha_inputs(input_names):
    inputs = ["query", "key", "value", "bias", "key_padding_mask", "relative_position_bias", "past_key", "past_value"]
    return input_names[:-2] + [""] * (len(inputs) - len(input_names)) + input_names[-2:]


def create_multi_head_attention_onnx_model(config: MultiHeadAttentionConfig):
    input_names, output_names = config.get_input_output_names()

    float_type = TensorProto.FLOAT16 if config.dtype == torch.float16 else TensorProto.FLOAT
    nodes = [
        helper.make_node(
            "MultiHeadAttention",
            fill_optional_mha_inputs(input_names) if config.use_kv_cache else input_names,
            output_names,
            "MultiHeadAttention_0",
            num_heads=config.num_heads,
            unidirectional=int(config.causal),
            scale=config.softmax_scale,
            domain="com.microsoft",
        ),
    ]

    shape_dict = config.shape_dict()
    inputs = [
        helper.make_tensor_value_info(input_name, float_type, list(shape_dict[input_name]))
        for input_name in input_names
    ]

    outputs = [
        helper.make_tensor_value_info(output_name, float_type, list(shape_dict[output_name]))
        for output_name in output_names
    ]

    graph = helper.make_graph(
        nodes,
        "MultiHeadAttention_Graph",
        inputs,
        outputs,
    )

    model = helper.make_model(graph)

    return model.SerializeToString()


def create_session(config: MultiHeadAttentionConfig, session_options=None) -> CudaSession:
    onnx_model_str = create_multi_head_attention_onnx_model(config)

    if config.provider == "CUDAExecutionProvider":
        device_id = torch.cuda.current_device() if isinstance(config.device, str) else config.device.index
        provider_options = CudaSession.get_cuda_provider_options(device_id, config.enable_cuda_graph)
        providers = [(config.provider, provider_options), "CPUExecutionProvider"]
    else:
        providers = ["CPUExecutionProvider"]

    ort_session = InferenceSession(onnx_model_str, session_options, providers=providers)
    cuda_session = CudaSession(ort_session, config.device, config.enable_cuda_graph)
    shape_dict = config.shape_dict()
    cuda_session.allocate_buffers(shape_dict)
    return cuda_session


class OrtMultiHeadAttention:
    """A wrapper of ORT MultiHeadAttention to test relevance and performance."""

    def __init__(self, config: MultiHeadAttentionConfig, session_options=None):
        self.ort_session = create_session(config, session_options)
        self.feed_dict = config.random_inputs()

    def infer(self):
        return self.ort_session.infer(self.feed_dict)


def measure_latency(cuda_session: CudaSession, input_dict):
    start = time.time()
    _ = cuda_session.infer(input_dict)
    end = time.time()
    return end - start


def flops(batch, sequence_length, head_size, num_heads, causal):
    return 4 * batch * sequence_length**2 * num_heads * head_size // (2 if causal else 1)


def tflops_per_second(flop, time):
    return (flop / time / 10**12) if not math.isnan(time) else 0.0


def get_gpu_kernel_name(config: MultiHeadAttentionConfig) -> str:
    # This classification is for Nvidia GPU of Compute Capability 8.* like A100.
    # Note that some kernel might not exist in older or newer GPUs.
    if os.getenv("ORT_DISABLE_FLASH_ATTENTION") != "1":
        if config.input_format == InputFormats.QKV_BSN3H:
            min_seq_len = os.getenv("ORT_MIN_SEQ_LEN_FLASH_ATTENTION_PACKED_QKV")
            min_length = int(min_seq_len) if min_seq_len is not None else 513
            if config.sequence_length >= min_length:
                return "Flash"
        else:
            return "Flash"

    if (os.getenv("ORT_DISABLE_FUSED_CROSS_ATTENTION") != "1" and config.kv_sequence_length <= 128) or (
        os.getenv("ORT_DISABLE_FUSED_ATTENTION") != "1"
        and (config.sequence_length <= 384 or os.getenv("ORT_DISABLE_TRT_FLASH_ATTENTION") != "1")
    ):
        return "TRT"

    if os.getenv("ORT_DISABLE_MEMORY_EFFICIENT_ATTENTION") != "1":
        return "MemEff"

    return "Unfused"


def get_cpu_kernel_name(config: MultiHeadAttentionConfig) -> str:
    # CPU Flash Attention does not support causal and kv cache etc.
    if not (config.causal or config.use_kv_cache or config.past_sequence_length > 0):
        if os.getenv("ORT_DISABLE_FLASH_ATTENTION") != "1":
            return "CPU:Flash"

    return "CPU:Unfused"


def run_torch_mha(batch_size: int, q_seq_len: int, kv_seq_len: int, num_heads: int, head_size: int, causal:bool,
                  device, dtype, repeats: int = 100, has_mask:bool=False, mask_dim:int=2, bool_mask:bool=True):
    q_shape = (batch_size, num_heads, q_seq_len, head_size)
    kv_shape = (batch_size, num_heads, kv_seq_len, head_size)
    q = torch.randn(q_shape, device=device, dtype=dtype)
    k = torch.randn(kv_shape, device=device, dtype=dtype)
    v = torch.randn(kv_shape, device=device, dtype=dtype)

    attn_mask = None
    if has_mask:
        if mask_dim == 4:
            mask_shape = (batch_size, num_heads, q_seq_len, kv_seq_len)
        else:
            mask_shape = (q_seq_len, kv_seq_len)

        if bool_mask: #TODO: no random
            attn_mask = torch.randint(0, 2, size=mask_shape, dtype=torch.bool, device=device)
        else:
            attn_mask = torch.randn(mask_shape, dtype=dtype, device=device)

    def measure_torch_latency():
        start = time.time()
        with sdpa_kernel(SDPBackend.FLASH_ATTENTION):
            _ = scaled_dot_product_attention(q, k, v, attn_mask=attn_mask, dropout_p=0.0, is_causal=causal)
        end = time.time()
        return end - start

    # warm up session
    _ = measure_torch_latency()

    latency_list = []
    for _ in range(repeats):
        latency = measure_torch_latency()
        latency_list.append(latency)
    average_latency = statistics.mean(latency_list)

    return average_latency



def run_tflops_test(
    csv_writer: csv.DictWriter,
    use_gpu: bool = True,
    enable_cuda_graph: bool = False,
    causal: bool = False,
    use_kv_cache: bool = False,
    intra_op_num_threads: int = 0,
    repeats: int = 100,
):
    if use_gpu:
        device_id = torch.cuda.current_device()
        device = torch.device("cuda", device_id)
        formats = [InputFormats.Q_K_V_BSNH_BSNH_BSNH, InputFormats.Q_KV_BSNH_BSN2H, InputFormats.QKV_BSN3H]
        provider = "CUDAExecutionProvider"
        print(f"enable_cuda_graph={enable_cuda_graph}")
    else:
        device_id = 0
        device = torch.device("cpu")
        formats = [InputFormats.Q_K_V_BSNH_BSNH_BSNH]
        enable_cuda_graph = False
        provider = "CPUExecutionProvider"

    if use_gpu:
        # (batch_size, sequence_length, past_sequence_length, num_heads, head_size, run_unfused)
        configs = [
            (32, 512, 0, 64, 32, True),
            (32, 512, 0, 128, 16, True),
            (16, 1024, 0, 64, 32, True),
            (16, 1024, 0, 128, 16, True),
            (8, 2048, 0, 64, 32, True),
            (8, 2048, 0, 128, 16, False),
            (4, 4096, 0, 64, 32, False),
            (4, 4096, 0, 128, 16, False),
            (2, 8192, 0, 64, 32, False),
            (2, 8192, 0, 128, 16, False),
            (1, 16384, 0, 64, 32, False),
            (1, 16384, 0, 128, 16, False),
            # stable diffusion
            (1, 4096, 0, 8, 40, False),
            (1, 4096, 0, 8, 80, False),
            (1, 4096, 0, 8, 160, False),
            (4, 4096, 0, 8, 40, False),
            (4, 4096, 0, 8, 80, False),
            (4, 4096, 0, 8, 160, False),
            (1, 16384, 0, 8, 40, False),
            (1, 16384, 0, 8, 80, False),
            (1, 16384, 0, 8, 160, False),
            # bert-base
            (128, 128, 0, 12, 64, True),
            (64, 128, 0, 12, 64, True),
            (128, 384, 0, 12, 64, True),
            (64, 384, 0, 12, 64, True),
            (128, 512, 0, 12, 64, True),
            (64, 512, 0, 12, 64, True),
            # TNLGv4
            (4, 2048, 0, 32, 128, True),
            (4, 4096, 0, 32, 128, False),
            (8, 2048, 0, 32, 128, False),
            (8, 4096, 0, 32, 128, False),
        ]
    else:
        configs = [
            # TNLGv4
            (1, 128, 0, 32, 128, True),
            (1, 256, 0, 32, 128, True),
            (1, 512, 0, 32, 128, True),
            (1, 1024, 0, 32, 128, True),
            (1, 2048, 0, 32, 128, True),
            # bert-base
            (1, 128, 0, 12, 64, True),
            (1, 384, 0, 12, 64, True),
            (1, 512, 0, 12, 64, True),
            (4, 128, 0, 12, 64, True),
            (4, 384, 0, 12, 64, True),
            (4, 512, 0, 12, 64, True),
            # bert-large
            (1, 128, 0, 16, 64, True),
            (1, 384, 0, 16, 64, True),
            (1, 512, 0, 16, 64, True),
            (4, 128, 0, 16, 64, True),
            (4, 384, 0, 16, 64, True),
            (4, 512, 0, 16, 64, True),
        ]

    # List of environment variables to enable/disable attention kernels
    print("Environment Variables:")
    env_names = [
        "ORT_DISABLE_FLASH_ATTENTION",
        "ORT_MIN_SEQ_LEN_FLASH_ATTENTION_PACKED_QKV",
        "ORT_DISABLE_FUSED_ATTENTION",
        "ORT_DISABLE_TRT_FLASH_ATTENTION",
        "ORT_ENABLE_FUSED_CAUSAL_ATTENTION",
        "ORT_DISABLE_FUSED_CROSS_ATTENTION",
        "ORT_DISABLE_MEMORY_EFFICIENT_ATTENTION",
    ]

    env_list = ""
    for name in env_names:
        value = os.getenv(name)
        if value is not None:
            print(f"{name}={value}")
            if env_list:
                env_list += ","
            env_list += f"{name}={value}"

    print("\nformat\tcausal\tbatch\tseqlen\theads\th_dim\tthreads\tms\tTFLOPS\tkernel")

    for input_format in formats:
        for batch_size, sequence_length, past_sequence_length, num_heads, head_size, enable_unfused in configs:
            config = MultiHeadAttentionConfig(
                batch_size=batch_size,
                sequence_length=sequence_length,
                num_heads=num_heads,
                head_size=head_size,
                causal=causal,
                use_kv_cache=use_kv_cache,
                past_sequence_length=past_sequence_length,
                max_cache_sequence_length=None,
                kv_sequence_length=None,
                provider=provider,
                enable_cuda_graph=enable_cuda_graph,
                device=device,
                dtype=torch.float16 if use_gpu else torch.float,
                share_past_present_buffer=False,
                input_format=input_format,
            )

            sess_options = SessionOptions()
            sess_options.intra_op_num_threads = intra_op_num_threads
            session = create_session(config, sess_options)

            if use_gpu:
                kernel = get_gpu_kernel_name(config)
            else:
                kernel = get_cpu_kernel_name(config)

            if kernel == "Unfused":
                # Skip large sequence length for Unfused kernel to avoid OOM.
                if not enable_unfused:
                    continue

                # Unfused kernel does not support packed QKV or packed KV formats.
                if input_format not in [InputFormats.Q_K_V_BSNH_BSNH_BSNH]:
                    continue

            input_dict = config.random_inputs()

            # warm up session
            _ = measure_latency(session, input_dict)

            latency_list = []
            for _ in range(repeats):
                latency = measure_latency(session, input_dict)
                latency_list.append(latency)
            average_latency = statistics.mean(latency_list)

            del session

            # compute TFLOPS per second
            speed = tflops_per_second(flops(batch_size, sequence_length, head_size, num_heads, causal), average_latency)

            format = InputFormats.input_format_str(input_format)
            print(
                f"{format}\t{causal}\t{batch_size}\t{sequence_length}\t{num_heads}\t{head_size}\t"
                f"{intra_op_num_threads}\t{average_latency * 1000:.2f}\t{speed:.2f}\t{kernel}"
            )

            row = {
                "use_gpu": use_gpu,
                "enable_cuda_graph": enable_cuda_graph,
                "format": format,
                "causal": causal,
                "batch_size": batch_size,
                "sequence_length": sequence_length,
                "past_sequence_length": past_sequence_length,
                "num_heads": num_heads,
                "head_size": head_size,
                "intra_op_num_threads": intra_op_num_threads,
                "average_latency": average_latency,
                "tflops": speed,
                "kernel": kernel,
                "environment_variables": env_list,
            }
            csv_writer.writerow(row)

            # and Version(torch.__version__) >= Version("2.5.0")
            if intra_op_num_threads == 0 and not (use_gpu or use_kv_cache or input_format != InputFormats.Q_K_V_BSNH_BSNH_BSNH):
                with torch.no_grad():
                    torch_latency = run_torch_flash_mha(batch_size, sequence_length, sequence_length, num_heads, head_size,
                                                        causal, has_mask=False, mask_dim=2, bool_mask=False,
                                                        device=device, dtype=torch.float32)
                speed = tflops_per_second(flops(batch_size, sequence_length, head_size, num_heads, causal), torch_latency)
                kernel="Torch:Flash"
                print(
                    f"{format}\t{causal}\t{batch_size}\t{sequence_length}\t{num_heads}\t{head_size}\t"
                    f"{intra_op_num_threads}\t{torch_latency * 1000:.2f}\t{speed:.2f}\t{kernel}"
                )
                row = {
                    "use_gpu": use_gpu,
                    "enable_cuda_graph": enable_cuda_graph,
                    "format": format,
                    "causal": causal,
                    "batch_size": batch_size,
                    "sequence_length": sequence_length,
                    "past_sequence_length": past_sequence_length,
                    "num_heads": num_heads,
                    "head_size": head_size,
                    "intra_op_num_threads": intra_op_num_threads,
                    "average_latency": torch_latency,
                    "tflops": speed,
                    "kernel": kernel,
                    "environment_variables": "",
                }
                csv_writer.writerow(row)

def run_tflops_tests(
    use_gpu: bool = True,
    enable_cuda_graph: bool = False,
    test_threads:bool=False
):
    csv_filename = "benchmark_mha_{}_{}.csv".format(
        "gpu" if use_gpu else "cpu", datetime.now().strftime("%Y%m%d-%H%M%S")
    )
    with open(csv_filename, mode="a", newline="") as csv_file:
        column_names = [
            "use_gpu",
            "enable_cuda_graph",
            "format",
            "causal",
            "batch_size",
            "sequence_length",
            "past_sequence_length",
            "num_heads",
            "head_size",
            "intra_op_num_threads",
            "average_latency",
            "tflops",
            "kernel",
            "environment_variables",
        ]
        csv_writer = csv.DictWriter(csv_file, fieldnames=column_names)
        csv_writer.writeheader()

        for causal, use_kv_cache in [(False, False), (True, True)]:
            for intra_op_num_threads in [1, 2, 4, 8, 16] if test_threads else [0]:  # 0 means using all CPU cores by default.
                run_tflops_test(csv_writer, use_gpu, enable_cuda_graph, causal, use_kv_cache, intra_op_num_threads)


def plot_prompt_performance(
    sm: int,
    model_name: str,
    batch_size: int,
    num_heads: int,
    head_size: int,
    max_seq_len: int,
):
    import triton

    formats = InputFormats.get_name_list()

    # Exclude cross attention since kernel crashes for some configuration.
    formats = formats[:-1]

    settings = {
        "line_vals": formats,
        "line_names": ["ORT-MHA:" + name for name in formats],
        "styles": [("red", "solid"), ("yellow", "dashdot"), ("blue", "dashed"), ("green", "dotted")][0 : len(formats)],
    }

    configs = [
        triton.testing.Benchmark(
            x_names=["sequence_length"],
            x_vals=[2**i for i in range(6, 17) if 2**i <= max_seq_len],
            line_arg="input_format",
            ylabel="ms",
            **settings,
            plot_name=f"prompt-sm{sm}-{model_name}-b{batch_size}-h{num_heads}_{head_size}-fp16",
            args={
                "batch_size": batch_size,
                "num_heads": num_heads,
                "head_size": head_size,
            },
        )
    ]

    @triton.testing.perf_report(configs)
    def benchmark(
        input_format: str,
        sequence_length: int,
        batch_size: int,
        num_heads: int,
        head_size: int,
        device="cuda",
    ):
        warmup = 15
        repeat = 100

        config: MultiHeadAttentionConfig = MultiHeadAttentionConfig(
            batch_size=batch_size,
            sequence_length=sequence_length,
            num_heads=num_heads,
            head_size=head_size,
            causal=True,
            past_sequence_length=0,
            kv_sequence_length=sequence_length if input_format == InputFormats.get_name_list()[-1] else None,
            max_cache_sequence_length=max_seq_len,
            provider="CUDAExecutionProvider",
            enable_cuda_graph=False,
            device=device,
            use_kv_cache=False,
            input_format=InputFormats.convert(input_format),
        )

        obj = OrtMultiHeadAttention(config)
        ms = triton.testing.do_bench(obj.infer, warmup=warmup, rep=repeat)
        return ms

    benchmark.run(save_path=".", print_data=True)


def run_causal_performance_test(sm: int):
    """
    Run performance tests for prompt and token generation.
    """
    configures = [
        (1, 32, 128, 8192, "TNLGv4"),
        (4, 32, 128, 8192, "TNLGv4"),
        (1, 12, 64, 1024, "BertBase"),
        (16, 12, 64, 1024, "BertBase"),
        (1, 16, 64, 1024, "BertLarge"),
        (8, 16, 64, 1024, "BertLarge"),
    ]

    for batch_size, num_heads, head_size, max_seq_len, model_name in configures:
        plot_prompt_performance(
            sm=sm,
            batch_size=batch_size,
            num_heads=num_heads,
            head_size=head_size,
            max_seq_len=max_seq_len,
            model_name=model_name,
        )

if __name__ == "__main__":
    if torch.cuda.is_available() and "CUDAExecutionProvider" in get_available_providers():
        # Test CUDA provider
        major, minor = torch.cuda.get_device_capability()
        sm = major * 10 + minor

        if platform.system() == "Linux":
            s = torch.cuda.Stream()
            with torch.cuda.stream(s), torch.no_grad():
                run_causal_performance_test(sm)

        run_tflops_tests(use_gpu=True, enable_cuda_graph=True)

    # Test CPU provider
    run_tflops_tests(use_gpu=False, enable_cuda_graph=False)
