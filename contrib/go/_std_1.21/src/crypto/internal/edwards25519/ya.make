GO_LIBRARY()
IF (OS_DARWIN AND ARCH_ARM64 OR OS_DARWIN AND ARCH_X86_64 OR OS_LINUX AND ARCH_AARCH64 OR OS_LINUX AND ARCH_X86_64 OR OS_WINDOWS AND ARCH_X86_64)
    SRCS(
		doc.go
		edwards25519.go
		scalar.go
		scalar_fiat.go
		scalarmult.go
		tables.go
    )
ENDIF()
END()


RECURSE(
	field
)
