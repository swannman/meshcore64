;
; NMI-driven ACIA receive for SwiftLink at $DE00
;
; Hooks the C64 NMI vector ($0318) to check the ACIA on every
; non-maskable interrupt.  When data is available, stores it in
; a 256-byte ring buffer before returning via RTI.
;
; The ACIA generates NMI when RDRF is set (with VICE -acia1irq 1).
; NMI is edge-triggered on the 6502: each new byte arrival
; triggers exactly one NMI.  Reading ACIA_DATA clears RDRF and
; re-arms the NMI for the next byte.
;
; If the NMI was not caused by the ACIA (e.g. RESTORE key),
; we chain to the old NMI handler.
;

        .export _nmi_rx_init
        .export _nmi_rx_buf
        .export _nmi_rx_head
        .export _nmi_rx_tail

        .segment "BSS"

_nmi_rx_buf:    .res 256        ; Ring buffer (256 bytes, wraps naturally)
_nmi_rx_head:   .res 1          ; Write position (updated by NMI handler)
_nmi_rx_tail:   .res 1          ; Read position (updated by main code)
old_nmi:        .res 2          ; Saved old NMI vector (lo, hi)

        .segment "CODE"

; ACIA registers
ACIA_DATA   = $DE00
ACIA_STATUS = $DE01
ACIA_CMD    = $DE02
ACIA_RDRF   = $08              ; Receive Data Register Full bit

; NMI vector in C64 RAM (NMINV)
NMINV       = $0318

;---------------------------------------------------------------
; void nmi_rx_init(void)
;
; Initialize the ring buffer, save old NMI vector, install our
; handler, and enable ACIA RX interrupts (NMI mode).
;---------------------------------------------------------------
_nmi_rx_init:
        ; Clear ring buffer pointers
        lda     #0
        sta     _nmi_rx_head
        sta     _nmi_rx_tail

        ; Drain any pending ACIA byte
        lda     ACIA_STATUS
        and     #ACIA_RDRF
        beq     @no_drain
        lda     ACIA_DATA       ; Discard
@no_drain:

        ; Save old NMI vector and install ours
        ; (SEI not strictly needed for NMI hookup, but keeps things clean)
        sei
        lda     NMINV
        sta     old_nmi
        lda     NMINV+1
        sta     old_nmi+1

        lda     #<nmi_handler
        sta     NMINV
        lda     #>nmi_handler
        sta     NMINV+1

        ; Enable ACIA RX interrupt:
        ; Command register: DTR active, RX IRQ enabled (bit1=0), TX no IRQ + RTS low
        ; Value $09 = 0000_1001: DTR on, RX int enabled, TX bits=10 (no IRQ, RTS low)
        lda     #$09
        sta     ACIA_CMD

        cli
        rts

;---------------------------------------------------------------
; NMI handler
;
; The KERNAL NMI entry at $FE43 does: SEI, JMP ($0318)
; Registers are NOT saved yet when we get control.
; We must save/restore A, X, Y ourselves.
;
; If ACIA has data (RDRF set), buffer it and RTI.
; If not (RESTORE key or other source), chain to old handler.
;---------------------------------------------------------------
nmi_handler:
        ; Save registers
        pha
        txa
        pha
        tya
        pha

        ; Check if ACIA has data
        lda     ACIA_STATUS
        and     #ACIA_RDRF
        beq     @not_acia

        ; ACIA has a byte — buffer it
        lda     ACIA_DATA       ; Read byte (clears RDRF, re-arms NMI)
        ldx     _nmi_rx_head
        sta     _nmi_rx_buf,x
        inx                     ; Wraps at 256 (8-bit X)
        stx     _nmi_rx_head

        ; Check again — might have another byte (burst mode)
        lda     ACIA_STATUS
        and     #ACIA_RDRF
        beq     @done
        lda     ACIA_DATA
        ldx     _nmi_rx_head
        sta     _nmi_rx_buf,x
        inx
        stx     _nmi_rx_head

@done:
        ; Restore registers and return from NMI
        pla
        tay
        pla
        tax
        pla
        rti

@not_acia:
        ; Not our interrupt (RESTORE key etc.) — chain to old handler
        ; Restore our saved registers first, then let old handler
        ; save its own and process
        pla
        tay
        pla
        tax
        pla
        jmp     (old_nmi)
