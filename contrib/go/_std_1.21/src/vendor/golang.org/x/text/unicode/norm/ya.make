GO_LIBRARY()
IF (OS_DARWIN AND ARCH_ARM64 AND RACE OR OS_DARWIN AND ARCH_ARM64 AND NOT RACE OR OS_DARWIN AND ARCH_X86_64 AND RACE OR OS_DARWIN AND ARCH_X86_64 AND NOT RACE OR OS_LINUX AND ARCH_AARCH64 AND RACE OR OS_LINUX AND ARCH_AARCH64 AND NOT RACE OR OS_LINUX AND ARCH_X86_64 AND RACE OR OS_LINUX AND ARCH_X86_64 AND NOT RACE OR OS_WINDOWS AND ARCH_X86_64 AND RACE OR OS_WINDOWS AND ARCH_X86_64 AND NOT RACE)
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
