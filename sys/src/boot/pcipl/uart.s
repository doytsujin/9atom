TEXT uartputc(SB), $0
	MOVL 4(SP),AX
	CALL rmode16(SB)
	STI
	CLR(rDX)
	MOVB $0x01, AH
	BIOSCALL(0x14)
	CALL16(pmode32(SB))
	XORL AX,AX
	RET

TEXT uartgotc(SB), $0
	CALL rmode16(SB)
	STI
	CLR(rDX)
	MOVB $0x03, AH
	BIOSCALL(0x14)
	CALL16(pmode32(SB))
	ANDL $0x8100,AX
	MOVL $0x0100,BX
	CMPL BX,AX
	JE uartread
	XORL AX,AX
	RET
uartread:
	CALL rmode16(SB)
	STI
	CLR(rDX)
	MOVB $0x02, AH
	BIOSCALL(0x14)
	CALL16(pmode32(SB))
	ANDL $0xFF,AX
	RET

