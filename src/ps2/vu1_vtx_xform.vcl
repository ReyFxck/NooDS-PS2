; VU1 Vertex Transform Microcode
; Transforms vertices by clip matrix (clip space output, no perspective divide)
; Input: Clip matrix at 0x0200, Vertex count at 0x0320, Vertices at 0x0400
; Output: Clip space vertices at 0x1000
; VF12-VF15 = Clip matrix columns, VF20 = input vertex, VF21 = output vertex
; VI00 = 0 (base), VI01 = vertex count, VI02 = vertex index

.syntax new
.name VU1VtxXform
.vu
.init_vf_all
.init_vi_all

--enter
--endenter

    ; Initialize base address
    iaddiu  VI00, VI00, 0

    ; Load clip matrix (4 columns)
    lq      VF12, 0x200(VI00)  ; Clip col 0
    lq      VF13, 0x210(VI00)  ; Clip col 1
    lq      VF14, 0x220(VI00)  ; Clip col 2
    lq      VF15, 0x230(VI00)  ; Clip col 3

    ; Load vertex count
    ilw.w   VI01, 0x320(VI00)  ; VI01 = vertex_count
    iaddiu  VI02, VI00, 0      ; VI02 = vertex index (0)

vtx_loop:
    ; Check if done (branch if VI02 == VI01 to vtx_done)
    ibeq    VI01, VI02, vtx_done
    nop

    ; Load vertex (x, y, z, w) - 16 bytes per vertex
    lq      VF20, 0x400(VI02)

    ; Transform: v' = Clip * v (clip space, keep w for later perspective divide)
    mul     ACC, VF12, VF20[x]
    madd    ACC, VF13, VF20[y]
    madd    ACC, VF14, VF20[z]
    madd    VF21, VF15, VF20[w]

    ; Store clip space vertex (x, y, z, w)
    sq      VF21, 0x1000(VI02)

    ; Next vertex (16 bytes = 1 qword)
    iaddiu  VI02, VI02, 16
    ibne    VI01, VI02, vtx_loop

vtx_done:
    nop
    nop

--exit
--endexit