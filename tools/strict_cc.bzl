"""Strict wrappers for first-party C++ targets."""

load("@rules_cc//cc:cc_binary.bzl", _cc_binary = "cc_binary")
load("@rules_cc//cc:cc_library.bzl", _cc_library = "cc_library")
load("@rules_cc//cc:cc_test.bzl", _cc_test = "cc_test")

_STRICT_COPTS = [
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
]

def _copts(copts, strict_warnings):
    return _STRICT_COPTS + copts if strict_warnings else copts

def cc_binary(name, copts = [], strict_warnings = True, **kwargs):
    """Defines a first-party C++ binary with strict warnings by default."""
    _cc_binary(
        name = name,
        copts = _copts(copts, strict_warnings),
        **kwargs
    )

def cc_library(name, copts = [], strict_warnings = True, **kwargs):
    """Defines a first-party C++ library with strict warnings by default."""
    _cc_library(
        name = name,
        copts = _copts(copts, strict_warnings),
        **kwargs
    )

def cc_test(name, copts = [], strict_warnings = True, **kwargs):
    """Defines a first-party C++ test with strict warnings by default."""
    _cc_test(
        name = name,
        copts = _copts(copts, strict_warnings),
        **kwargs
    )
