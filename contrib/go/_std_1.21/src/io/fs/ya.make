GO_LIBRARY()
IF (OS_DARWIN AND ARCH_ARM64 OR OS_DARWIN AND ARCH_X86_64 OR OS_LINUX AND ARCH_AARCH64 OR OS_LINUX AND ARCH_X86_64 OR OS_WINDOWS AND ARCH_X86_64)
    SRCS(
		format.go
		fs.go
		glob.go
		readdir.go
		readfile.go
		stat.go
		sub.go
		walk.go
    )
ENDIF()
END()
