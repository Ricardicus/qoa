load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

QOA_LINUX_WARNING_FLAGS = [
    "-Wall",
    "-Wextra",
    "-pedantic-errors",
    "-Werror",
    "-Wdouble-promotion",
    "-Wformat=2",
    "-Wmissing-declarations",
    "-Wnull-dereference",
    "-Wshadow",
    "-Wsign-compare",
    "-Wundef",
    "-fno-common",
    "-Wnon-virtual-dtor",
    "-Woverloaded-virtual",
    # Common idiom for zeroing members.
    "-Wno-missing-field-initializers",
]

QOA_MSVC_WARNING_FLAGS = [
    # More warnings.
    "/W4",
    # Treat warnings as errors.
    "/WX",
]

QOA_COPTS = select({
    "@platforms//os:linux": QOA_LINUX_WARNING_FLAGS,
    "@platforms//os:windows": QOA_MSVC_WARNING_FLAGS,
})

cc_library(
    name = "qoa",
    srcs = glob(
        include = ["*.cpp"],
        exclude = [
            "*_example.cpp",
            "*_test.cpp",
        ],
    ),
    hdrs = glob(["*.h"]),
    copts = QOA_COPTS,
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "qoa_example",
    srcs = ["qoa_example.cpp"],
    copts = QOA_COPTS,
    deps = [
        ":qoa",
    ],
)
