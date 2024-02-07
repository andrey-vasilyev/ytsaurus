GO_LIBRARY()
IF (OS_LINUX AND ARCH_AARCH64 AND RACE OR OS_LINUX AND ARCH_AARCH64 AND NOT RACE)
    SRCS(
		asm_linux_arm64.s
		defs_linux_arm64.go
		syscall_linux.go
    )
ELSEIF (OS_LINUX AND ARCH_X86_64 AND RACE OR OS_LINUX AND ARCH_X86_64 AND NOT RACE)
    SRCS(
		asm_linux_amd64.s
		defs_linux_amd64.go
		syscall_linux.go
    )
ENDIF()
END()
