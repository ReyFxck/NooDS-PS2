; VU1 Matrix Multiply Microcode
; C = A * B (4x4 column-major matrices)
; Input: A at 0x0100, B at 0x0140
; Output: C at 0x0180
; VF00-VF03 = A columns, VF04-VF07 = B columns, VF08-VF11 = C columns
; VI00 = 0 (base address)

.syntax new
.name VU1MtxMul
.vu
.init_vf_all
.init_vi_all

--enter
--endenter

    ; Initialize base address
    iaddiu  VI00, VI00, 0

    ; Load Matrix A (4 columns)
    lq      VF00, 0x100(VI00)  ; A col 0
    lq      VF01, 0x110(VI00)  ; A col 1
    lq      VF02, 0x120(VI00)  ; A col 2
    lq      VF03, 0x130(VI00)  ; A col 3

    ; Load Matrix B (4 columns)
    lq      VF04, 0x140(VI00)  ; B col 0
    lq      VF05, 0x150(VI00)  ; B col 1
    lq      VF06, 0x160(VI00)  ; B col 2
    lq      VF07, 0x170(VI00)  ; B col 3

    ; Result col 0 = A * B_col0
    mul     ACC, VF00, VF04[x]
    madd    ACC, VF01, VF04[y]
    madd    ACC, VF02, VF04[z]
    madd    VF08, VF03, VF04[w]

    ; Result col 1 = A * B_col1
    mul     ACC, VF00, VF05[x]
    madd    ACC, VF01, VF05[y]
    madd    ACC, VF02, VF05[z]
    madd    VF09, VF03, VF05[w]

    ; Result col 2 = A * B_col2
    mul     ACC, VF00, VF06[x]
    madd    ACC, VF01, VF06[y]
    madd    ACC, VF02, VF06[z]
    madd    VF10, VF03, VF06[w]

    ; Result col 3 = A * B_col3
    mul     ACC, VF00, VF07[x]
    madd    ACC, VF01, VF07[y]
    madd    ACC, VF02, VF07[z]
    madd    VF11, VF03, VF07[w]

    ; Store result
    sq      VF08, 0x180(VI00)
    sq      VF09, 0x190(VI00)
    sq      VF10, 0x1A0(VI00)
    sq      VF11, 0x1B0(VI00)

    nop
    nop

--exit
--endexit