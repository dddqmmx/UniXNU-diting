/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_USB_COMMAND_H
#define DITING_USB_COMMAND_H
#include <stdint.h>
#include <string.h>
/* Decimal cursor and bounded record count, independent of any RAM address. */
static inline int diting_usb_parse_trace(const char *line, uint64_t *after, unsigned *records)
{
    if (!line || !after || !records || strncmp(line, "trace ", 6)) return 0;
    const char *at=line+6;uint64_t cursor=0;unsigned digits=0;
    for (;*at && *at!=' ';++at) {
        if (*at<'0' || *at>'9' || cursor>(UINT64_MAX-(unsigned)(*at-'0'))/10)return 0;
        cursor=cursor*10+(unsigned)(*at-'0');++digits;
    }
    if (!digits || *at!=' ' || cursor==UINT64_MAX)return 0;
    ++at;unsigned count=0;
    if (!*at)return 0;
    for (;*at;++at) {
        if (*at<'0' || *at>'9' || count>2)return 0;
        count=count*10+(unsigned)(*at-'0');
        if (count>20)return 0;
    }
    if (!count)return 0;
    *after=cursor;*records=count;return 1;
}

static inline unsigned diting_usb_base64(const unsigned char *in,unsigned bytes,char *out,unsigned capacity)
{
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (bytes>3072)return 0;
    const unsigned length=((bytes+2)/3)*4;
    if ((!in && bytes) || !out || length>=capacity)return 0;
    unsigned used=0;
    for (unsigned i=0;i<bytes;i+=3) {
        const unsigned remain=bytes-i;
        uint32_t word=(uint32_t)in[i]<<16;
        if (remain>1)word|=(uint32_t)in[i+1]<<8;
        if (remain>2)word|=in[i+2];
        out[used++]=alphabet[(word>>18)&63];out[used++]=alphabet[(word>>12)&63];
        out[used++]=remain>1?alphabet[(word>>6)&63]:'=';
        out[used++]=remain>2?alphabet[word&63]:'=';
    }
    out[used]=0;return used;
}
static inline int diting_usb_parse_read(const char *line, uint64_t *address, unsigned *bytes)
{
    if (!line || !address || !bytes || strncmp(line, "read ", 5)) return 0;
    const char *at = line + 5;
    if (at[0] == '0' && (at[1] == 'x' || at[1] == 'X')) at += 2;
    uint64_t value = 0; unsigned digits = 0;
    for (; *at && *at != ' '; ++at) {
        unsigned digit;
        if (*at >= '0' && *at <= '9') digit = (unsigned)(*at - '0');
        else if (*at >= 'a' && *at <= 'f') digit = (unsigned)(*at - 'a') + 10;
        else if (*at >= 'A' && *at <= 'F') digit = (unsigned)(*at - 'A') + 10;
        else return 0;
        if (value > (UINT64_MAX - digit) / 16) return 0;
        value = value * 16 + digit; ++digits;
    }
    if (!digits || *at != ' ') return 0;
    ++at; unsigned amount = 0;
    if (!*at) return 0;
    for (; *at; ++at) {
        if (*at < '0' || *at > '9' || amount > 512 / 10) return 0;
        amount = amount * 10 + (unsigned)(*at - '0');
        if (amount > 512) return 0;
    }
    if (!amount || value > UINT64_MAX - amount) return 0;
    *address = value; *bytes = amount; return 1;
}
#endif
