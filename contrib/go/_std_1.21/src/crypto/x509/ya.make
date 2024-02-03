GO_LIBRARY()
IF (OS_DARWIN AND ARCH_ARM64 OR OS_DARWIN AND ARCH_X86_64)
    SRCS(
		cert_pool.go
		notboring.go
		parser.go
		pem_decrypt.go
		pkcs1.go
		pkcs8.go
		root.go
		root_darwin.go
		sec1.go
		verify.go
		x509.go
    )
ELSEIF (OS_LINUX AND ARCH_AARCH64 OR OS_LINUX AND ARCH_X86_64)
    SRCS(
		cert_pool.go
		notboring.go
		parser.go
		pem_decrypt.go
		pkcs1.go
		pkcs8.go
		root.go
		root_linux.go
		root_unix.go
		sec1.go
		verify.go
		x509.go
    )
ELSEIF (OS_WINDOWS AND ARCH_X86_64)
    SRCS(
		cert_pool.go
		notboring.go
		parser.go
		pem_decrypt.go
		pkcs1.go
		pkcs8.go
		root.go
		root_windows.go
		sec1.go
		verify.go
		x509.go
    )
ENDIF()
END()


RECURSE(
	internal
	pkix
)
