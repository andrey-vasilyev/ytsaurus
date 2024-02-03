GO_LIBRARY()
IF (OS_DARWIN AND ARCH_ARM64 OR OS_DARWIN AND ARCH_X86_64 OR OS_LINUX AND ARCH_AARCH64 OR OS_LINUX AND ARCH_X86_64 OR OS_WINDOWS AND ARCH_X86_64)
    SRCS(
		composition.go
		forminfo.go
		input.go
		iter.go
		normalize.go
		readwriter.go
		tables15.0.0.go
		transform.go
		trie.go
    )
ENDIF()
END()
