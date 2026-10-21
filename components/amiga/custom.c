/*
 * The custom chips' registers ($DFF000-$DFF1FE): reads, and writes from
 * the 68000 or the copper, passed on to the copper, the blitter and the
 * display; Paula's interrupt and Agnus's DMA control registers live here.
 */
#include <string.h>
#include "amiga_internal.h"

/* Agnus's ID in VPOSR: OCS 8370/8371, ECS 8372 (PAL, NTSC) */
static uint16_t agnus_id(void)
{
    return (ami.ecs ? 0x20 : 0x00) | (ami.ntsc ? 0x10 : 0x00);
}

static void beam(int *line, int *h)
{
    int cck = ami_now() / 2;
    *line = cck / CCK_LINE;
    *h = cck % CCK_LINE;
    if (*line >= ami.lines) {
        *line -= ami.lines;
    }
}

static uint16_t read_reg(uint32_t reg);

/* What the 68000 reads; a write-only register gives the last word on the
 * bus (Dr. Mario keeps a counter in one)
 */
uint16_t custom_read(uint32_t reg)
{
    uint16_t v = read_reg(reg);
    ami.bus = v;
    return v;
}

static uint16_t read_reg(uint32_t reg)
{
    int line, h;
    switch (reg) {
    case 0x002:                     /* DMACONR */
        return ami.dmacon | (blitter_busy(ami_now()) ? DMA_BBUSY : 0) | (blit.zero ? DMA_BZERO : 0);
    case 0x004:                     /* VPOSR */
        beam(&line, &h);
        return (ami.lof ? 0x8000 : 0) | (agnus_id() << 8) | ((line >> 8) & (ami.ecs ? 7 : 1));
    case 0x006:                     /* VHPOSR */
        beam(&line, &h);
        return ((line & 0xFF) << 8) | h;
    case 0x00A:                     /* JOY0DAT: the mouse */
        return input_joy0dat();
    case 0x00C:                     /* JOY1DAT: the joystick */
        return input_joy1dat();
    case 0x00E:                     /* CLXDAT: cleared by reading */
        return display_clxdat();
    case 0x010:                     /* ADKCONR */
        return ami.adkcon;
    case 0x012:                     /* POT0DAT, POT1DAT */
    case 0x014:
        return 0;
    case 0x016: {                   /* POTGOR: outputs as written, inputs pulled up;
                                       a button pulls a pin low either way (input.device
                                       drives them high: $FF00) */
        uint16_t v = 0;
        for (int pin = 0; pin < 4; pin++) {
            uint16_t out = 0x0200 << (pin * 2), dat = 0x0100 << (pin * 2);
            v |= (ami.potgo & out) ? (ami.potgo & dat) : dat;
        }
        return v & ~input_pot_low();
    }
    case 0x018:                     /* SERDATR: transmitter empty, line idle */
        return 0x3000 | 0x0800;
    case 0x01A:                     /* DSKBYTR */
        return floppy_dskbytr();
    case 0x01C:                     /* INTENAR */
        return ami.intena;
    case 0x01E:                     /* INTREQR */
        return ami.intreq;
    case 0x07C:                     /* DENISEID: ECS Denise 8373; OCS's isn't there */
        return ami.ecs ? 0xFFFC : ami.bus;
    default:
        return ami.bus;
    }
}

static uint16_t set_clr(uint16_t reg, uint16_t v, uint16_t mask)
{
    return (v & 0x8000) ? (reg | (v & mask)) : (reg & ~(v & mask));
}

void custom_write(uint32_t reg, uint16_t v, int at, bool copper)
{
    if (!copper) {
        ami.bus = v;
    }
    if (reg >= 0x040 && reg <= 0x074) {
        blitter_write(reg, v, at);
        return;
    }
    if ((reg >= 0x080 && reg <= 0x08C) || reg == 0x02E) {
        copper_write(reg, v, at, copper);
        return;
    }
    if (reg >= 0x0A0 && reg <= 0x0DE) {
        audio_write(reg, v);
        return;
    }
    if ((reg >= 0x08E && reg <= 0x094) || (reg >= 0x0E0 && reg <= 0x1BE) || reg == 0x1E4) {
        display_write(reg, v, at);
        return;
    }
    switch (reg) {
    case 0x020:                     /* DSKPTH, DSKPTL, DSKLEN, DSKSYNC */
    case 0x022:
    case 0x024:
    case 0x07E:
        floppy_write(reg, v);
        break;
    case 0x030:                     /* SERDAT: sent at once (nothing on the line) */
        if (ami.serial) {
            ami.serial(v & 0xFF);
        }
        ami_set_intreq(INT_TBE);
        break;
    case 0x034:                     /* POTGO */
        ami.potgo = v;
        break;
    case 0x036:                     /* JOYTEST */
        input_joytest(v);
        break;
    case 0x02A:                     /* VPOSW: the long frame bit */
        ami.lof = (v & 0x8000) != 0;
        break;
    case 0x096: {                   /* DMACON */
        uint16_t old = ami.dmacon;
        ami.dmacon = set_clr(ami.dmacon, v, 0x07FF);
        if (old != ami.dmacon) {
            copper_dma_changed(at);
            blitter_dma_changed(at);
            display_dma_changed(at);
            audio_dma_changed();
        }
        break;
    }
    case 0x098:                     /* CLXCON */
        display_clxcon(v);
        break;
    case 0x09A:                     /* INTENA */
        ami.intena = set_clr(ami.intena, v, 0x7FFF);
        ami_update_irq();
        break;
    case 0x09C:                     /* INTREQ: the CIAs' (and Gayle's) lines stay asserted */
        ami.intreq = set_clr(ami.intreq, v, 0x7FFF);
        if (cia[0].irq || gayle_irq()) {
            ami.intreq |= INT_PORTS;
        }
        if (cia[1].irq) {
            ami.intreq |= INT_EXTER;
        }
        ami_update_irq();
        break;
    case 0x09E:                     /* ADKCON */
        ami.adkcon = set_clr(ami.adkcon, v, 0x7FFF);
        break;
    case 0x1DC:                     /* BEAMCON0 (ECS) */
        if (ami.ecs) {
            ami.beamcon0 = v;
        }
        break;
    default:
        break;
    }
}

void custom_reset(void)
{
    ami.intena = ami.intreq = 0;
    ami.dmacon = ami.adkcon = 0;
    ami.potgo = 0;
    ami.beamcon0 = ami.ntsc ? 0 : 0x0020;
    ami_update_irq();
}
