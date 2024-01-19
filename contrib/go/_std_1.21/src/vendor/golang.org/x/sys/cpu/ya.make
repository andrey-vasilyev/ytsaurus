GO_LIBRARY()
IF (FALSE)
    MESSAGE(FATAL this shall never happen)

ELSEIF (OS_LINUX AND ARCH_X86_64)
    SRCS(
		byteorder.go
		cpu.go
		cpu_gc_x86.go
		cpu_linux_noinit.go
		cpu_x86.go
		cpu_x86.s
		endian_little.go
		hwcap_linux.go
		parse.go
		runtime_auxv.go
		runtime_auxv_go121.go
    )
ELSEIF (OS_LINUX AND ARCH_ARM64)
    SRCS(
		byteorder.go
		cpu.go
		cpu_arm64.go
		cpu_arm64.s
		cpu_gc_arm64.go
		cpu_linux_arm64.go
		endian_little.go
		hwcap_linux.go
		parse.go
		proc_cpuinfo_linux.go
		runtime_auxv.go
		runtime_auxv_go121.go
    )
ELSEIF (OS_LINUX AND ARCH_AARCH64)
    SRCS(
		byteorder.go
		cpu.go
		cpu_arm64.go
		cpu_arm64.s
		cpu_gc_arm64.go
		cpu_linux_arm64.go
		endian_little.go
		hwcap_linux.go
		parse.go
		proc_cpuinfo_linux.go
		runtime_auxv.go
		runtime_auxv_go121.go
    )
ELSEIF (OS_DARWIN AND ARCH_X86_64)
    SRCS(
		byteorder.go
		cpu.go
		cpu_gc_x86.go
		cpu_x86.go
		cpu_x86.s
		endian_little.go
		parse.go
		runtime_auxv.go
		runtime_auxv_go121.go
    )
ELSEIF (OS_DARWIN AND ARCH_ARM64)
    SRCS(
		byteorder.go
		cpu.go
		cpu_arm64.go
		cpu_arm64.s
		cpu_gc_arm64.go
		cpu_other_arm64.go
		endian_little.go
		parse.go
		runtime_auxv.go
		runtime_auxv_go121.go
    )
ELSEIF (OS_DARWIN AND ARCH_AARCH64)
    SRCS(
		byteorder.go
		cpu.go
		cpu_arm64.go
		cpu_arm64.s
		cpu_gc_arm64.go
		cpu_other_arm64.go
		endian_little.go
		parse.go
		runtime_auxv.go
		runtime_auxv_go121.go
    )
ELSEIF (OS_WINDOWS AND ARCH_X86_64)
    SRCS(
		byteorder.go
		cpu.go
		cpu_gc_x86.go
		cpu_x86.go
		cpu_x86.s
		endian_little.go
		parse.go
		runtime_auxv.go
		runtime_auxv_go121.go
    )
ELSEIF (OS_WINDOWS AND ARCH_ARM64)
    SRCS(
		byteorder.go
		cpu.go
		cpu_arm64.go
		cpu_arm64.s
		cpu_gc_arm64.go
		cpu_other_arm64.go
		endian_little.go
		parse.go
		runtime_auxv.go
		runtime_auxv_go121.go
    )
ELSEIF (OS_WINDOWS AND ARCH_AARCH64)
    SRCS(
		byteorder.go
		cpu.go
		cpu_arm64.go
		cpu_arm64.s
		cpu_gc_arm64.go
		cpu_other_arm64.go
		endian_little.go
		parse.go
		runtime_auxv.go
		runtime_auxv_go121.go
    )
ENDIF()
END()
