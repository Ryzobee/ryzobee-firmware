#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_runtime.h"

static uint64_t clock_ms;
static bool virtual_devices, stop_after_write, stopped;
static struct { uint32_t id; int pin; unsigned level; } pins[16];
/* A named virtual serial peer, not an ESP-IDF driver mock. Each newly opened
 * port has a binary greeting and thereafter echoes the bytes it accepted. */
static struct {
    uint32_t id;
    int tx, rx;
    unsigned error_events;
    size_t length;
    uint8_t bytes[256];
} serial_ports[2];
static struct {
    uint32_t id, period_ms;
    uint64_t next_ms;
    bool periodic;
} timers[16];
/* Virtual test wiring: PWM GPIO13 drives the GPIO18 input. This permits
 * observations through the same public Lua GPIO API rather than fixture state. */
static struct {
    uint32_t id, frequency_hz;
    uint64_t start_ms;
    int pin;
    unsigned duty;
} pwm_outputs[4];
static struct {
    uint32_t id;
    int sda, scl;
    uint8_t registers[256];
} i2c_buses[2];
static struct {
    uint32_t id;
    int sclk, mosi, miso, cs;
    size_t length;
    uint8_t bytes[256];
} spi_peer;
static struct { uint32_t id; int pin; } analog_inputs[6];
static uint32_t next_id;
static struct { uint32_t id; uint64_t due; uint8_t next[3], color[3]; bool wrote; } led_peer;
static bool led_close_busy, led_fail;
static ryz_peripheral_result_t virtual_led(const ryz_peripheral_request_t *r,
    ryz_peripheral_reply_t *out)
{
    if(r->op==RYZ_PERIPHERAL_OPEN) {
        if(led_peer.id) return RYZ_PERIPHERAL_BUSY;
        memset(&led_peer,0,sizeof(led_peer)); led_peer.id=++next_id;
        out->handle=led_peer.id; return RYZ_PERIPHERAL_OK;
    }
    if(!led_peer.id || led_peer.id!=r->handle) return RYZ_PERIPHERAL_CLOSED;
    if(r->op==RYZ_PERIPHERAL_CLOSE) {
        if(led_close_busy) { led_close_busy=false; return RYZ_PERIPHERAL_BUSY; }
        led_peer.id=0; return RYZ_PERIPHERAL_OK;
    }
    if(r->op==RYZ_PERIPHERAL_WRITE) {
        if(led_peer.wrote && clock_ms<led_peer.due) return RYZ_PERIPHERAL_BUSY;
        assert(r->length==3); memcpy(led_peer.next,r->data,3);
        led_peer.wrote=true; led_peer.due=clock_ms+20;
        stopped=stop_after_write; return RYZ_PERIPHERAL_OK;
    }
    if(r->op!=RYZ_PERIPHERAL_STATUS) return RYZ_PERIPHERAL_UNSUPPORTED;
    out->led.state=!led_peer.wrote ? RYZ_PERIPHERAL_LED_IDLE :
        clock_ms<led_peer.due ? RYZ_PERIPHERAL_LED_PENDING :
        led_fail ? RYZ_PERIPHERAL_LED_FAILED : RYZ_PERIPHERAL_LED_READY;
    out->led.output_known=out->led.state==RYZ_PERIPHERAL_LED_READY;
    if(out->led.output_known) memcpy(led_peer.color,led_peer.next,3);
    out->led.red=led_peer.color[0]; out->led.green=led_peer.color[1]; out->led.blue=led_peer.color[2];
    return RYZ_PERIPHERAL_OK;
}
/* Explicit virtual logging source: deterministic severity/text framing and
 * finite subscriber buffers; no ESP logger or real system events are mocked. */
static struct { uint32_t id, dropped; unsigned level; size_t used; uint8_t data[1024]; } logs[16];
static ryz_peripheral_result_t virtual_log(const ryz_peripheral_request_t *r,
    ryz_peripheral_reply_t *out)
{
    if(r->op==RYZ_PERIPHERAL_OPEN) {
        for(unsigned i=0;i<16;++i) if(!logs[i].id) {
            memset(&logs[i],0,sizeof(logs[i])); logs[i].id=++next_id;
            logs[i].level=r->config.log.level; out->handle=logs[i].id;
            return RYZ_PERIPHERAL_OK;
        }
        return RYZ_PERIPHERAL_NO_MEMORY;
    }
    if(r->op==RYZ_PERIPHERAL_WRITE && !r->handle) {
        assert(r->config.log.level>=1 && r->config.log.level<=5);
        const uint8_t prefix[]={(uint8_t)"?EWIDV"[r->config.log.level],':'};
        for(unsigned i=0;i<16;++i) if(logs[i].id && r->config.log.level<=logs[i].level) {
            size_t n=r->length+3;
            if(logs[i].used+n>sizeof(logs[i].data)) { logs[i].dropped+=(uint32_t)n; continue; }
            memcpy(logs[i].data+logs[i].used,prefix,2); logs[i].used+=2;
            memcpy(logs[i].data+logs[i].used,r->data,r->length); logs[i].used+=r->length;
            logs[i].data[logs[i].used++]='\n';
        }
        out->length=r->length; stopped=stop_after_write;
        return RYZ_PERIPHERAL_OK;
    }
    for(unsigned i=0;i<16;++i) if(logs[i].id==r->handle) {
        if(r->op==RYZ_PERIPHERAL_CLOSE) logs[i].id=0;
        else if(r->op==RYZ_PERIPHERAL_READ) {
            out->length=r->read_length<logs[i].used ? r->read_length : logs[i].used;
            memcpy(out->data,logs[i].data,out->length);
            logs[i].used-=out->length; memmove(logs[i].data,logs[i].data+out->length,logs[i].used);
            out->dropped=logs[i].dropped; out->loss_possible=false;
        } else return RYZ_PERIPHERAL_UNSUPPORTED;
        return RYZ_PERIPHERAL_OK;
    }
    return RYZ_PERIPHERAL_CLOSED;
}
static ryz_peripheral_result_t virtual_spi(const ryz_peripheral_request_t *r,
    ryz_peripheral_reply_t *out)
{
    if (r->op==RYZ_PERIPHERAL_OPEN) {
        if (spi_peer.id) return RYZ_PERIPHERAL_BUSY;
        for (unsigned p=0; p<16; ++p) if (pins[p].id &&
            (pins[p].pin==r->config.spi.sclk || pins[p].pin==r->config.spi.mosi ||
             pins[p].pin==r->config.spi.miso || pins[p].pin==r->config.spi.cs))
            return RYZ_PERIPHERAL_BUSY;
        spi_peer.id=++next_id;
        spi_peer.sclk=r->config.spi.sclk;
        spi_peer.mosi=r->config.spi.mosi;
        spi_peer.miso=r->config.spi.miso;
        spi_peer.cs=r->config.spi.cs;
        spi_peer.length=4;
        memcpy(spi_peer.bytes,"SP\0I",4);
        out->handle=spi_peer.id;
        return RYZ_PERIPHERAL_OK;
    }
    if (!spi_peer.id || spi_peer.id!=r->handle) return RYZ_PERIPHERAL_CLOSED;
    if (r->op==RYZ_PERIPHERAL_CLOSE) spi_peer.id=0;
    else if (r->op==RYZ_PERIPHERAL_TRANSFER) {
        if (spi_peer.miso<0 || spi_peer.mosi<0) return RYZ_PERIPHERAL_UNSUPPORTED;
        /* Virtual full-duplex device inverts each transmitted bit. */
        out->length=r->length;
        for (size_t i=0; i<out->length; ++i) out->data[i]=(uint8_t)~r->data[i];
    } else if (r->op==RYZ_PERIPHERAL_WRITE) {
        if (spi_peer.mosi<0) return RYZ_PERIPHERAL_UNSUPPORTED;
        out->length=r->length<3 ? r->length : 3;
        if (out->length>sizeof(spi_peer.bytes)-spi_peer.length)
            out->length=sizeof(spi_peer.bytes)-spi_peer.length;
        memcpy(spi_peer.bytes+spi_peer.length,r->data,out->length);
        spi_peer.length+=out->length;
    } else if (r->op==RYZ_PERIPHERAL_READ) {
        if (spi_peer.miso<0) return RYZ_PERIPHERAL_UNSUPPORTED;
        size_t queued=r->read_length<spi_peer.length ? r->read_length : spi_peer.length;
        memcpy(out->data,spi_peer.bytes,queued);
        memset(out->data+queued,0xff,r->read_length-queued);
        spi_peer.length-=queued;
        memmove(spi_peer.bytes,spi_peer.bytes+queued,spi_peer.length);
        out->length=r->read_length;
    } else return RYZ_PERIPHERAL_UNSUPPORTED;
    return RYZ_PERIPHERAL_OK;
}
/* Explicit analogue fixture: pin13 has a calibrated 2048-count/1650mV source;
 * pin14 has a 1024-count source without calibration; pin15 is unavailable
 * during acquisition. No voltage or wiring is inferred for the real board. */
static ryz_peripheral_result_t virtual_adc(const ryz_peripheral_request_t *r,
    ryz_peripheral_reply_t *out)
{
    if (r->op==RYZ_PERIPHERAL_OPEN) {
        if (r->config.adc.pin<13 || r->config.adc.pin>18) return RYZ_PERIPHERAL_UNSUPPORTED;
        for (unsigned i=0; i<6; ++i)
            if (analog_inputs[i].id && analog_inputs[i].pin==r->config.adc.pin) return RYZ_PERIPHERAL_BUSY;
        for (unsigned i=0; i<16; ++i)
            if (pins[i].id && pins[i].pin==r->config.adc.pin) return RYZ_PERIPHERAL_BUSY;
        for (unsigned i=0; i<6; ++i) if (!analog_inputs[i].id) {
            analog_inputs[i].id=++next_id;
            analog_inputs[i].pin=r->config.adc.pin;
            out->handle=analog_inputs[i].id;
            return RYZ_PERIPHERAL_OK;
        }
        return RYZ_PERIPHERAL_NO_MEMORY;
    }
    for (unsigned i=0; i<6; ++i) if (analog_inputs[i].id==r->handle) {
        if (r->op==RYZ_PERIPHERAL_CLOSE) analog_inputs[i].id=0;
        else if (r->op==RYZ_PERIPHERAL_READ || r->op==RYZ_PERIPHERAL_READ_MV) {
            if (analog_inputs[i].pin==15) return RYZ_PERIPHERAL_TIMEOUT;
            if (r->op==RYZ_PERIPHERAL_READ_MV) {
                if (analog_inputs[i].pin==14) return RYZ_PERIPHERAL_UNSUPPORTED;
                out->value=1650;
            } else out->value=analog_inputs[i].pin==14 ? 1024 : 2048;
        } else return RYZ_PERIPHERAL_UNSUPPORTED;
        return RYZ_PERIPHERAL_OK;
    }
    return RYZ_PERIPHERAL_CLOSED;
}
/* The external virtual register device is at 0x50. Its register selector
 * resets on STOP, so nonzero register reads require a repeated START. 0x51
 * NACKs and 0x52 holds the bus until timeout. These are explicit peer fixtures,
 * not claims about devices fitted to a physical Ryzobee. */
static ryz_peripheral_result_t virtual_i2c(const ryz_peripheral_request_t *r,
    ryz_peripheral_reply_t *out)
{
    if (r->op==RYZ_PERIPHERAL_OPEN) {
        unsigned bus=r->config.i2c.board ? 0 : 1;
        if (i2c_buses[bus].id) return RYZ_PERIPHERAL_BUSY;
        if (!r->config.i2c.board) {
            for (unsigned p=0; p<16; ++p) if (pins[p].id &&
                (pins[p].pin==r->config.i2c.sda || pins[p].pin==r->config.i2c.scl))
                return RYZ_PERIPHERAL_BUSY;
        }
        i2c_buses[bus].id=++next_id;
        i2c_buses[bus].sda=r->config.i2c.sda;
        i2c_buses[bus].scl=r->config.i2c.scl;
        memset(i2c_buses[bus].registers,0,sizeof(i2c_buses[bus].registers));
        memcpy(i2c_buses[bus].registers,"RY\0Z\xff\x07\x08\x09",8);
        out->handle=i2c_buses[bus].id;
        return RYZ_PERIPHERAL_OK;
    }
    for (unsigned bus=0; bus<2; ++bus) if (i2c_buses[bus].id==r->handle) {
        if (r->op==RYZ_PERIPHERAL_CLOSE) {
            i2c_buses[bus].id=0;
            return RYZ_PERIPHERAL_OK;
        }
        if (!bus && r->op!=RYZ_PERIPHERAL_PROBE) return RYZ_PERIPHERAL_UNSUPPORTED;
        if (r->address==0x52) return RYZ_PERIPHERAL_TIMEOUT;
        if (r->address!=(bus ? 0x50U : 0x15U)) return RYZ_PERIPHERAL_NOT_FOUND;
        if (r->op==RYZ_PERIPHERAL_PROBE) return RYZ_PERIPHERAL_OK;
        if (r->op==RYZ_PERIPHERAL_WRITE) {
            if (!r->length) return RYZ_PERIPHERAL_INVALID;
            unsigned offset=r->data[0];
            for (size_t i=1; i<r->length; ++i)
                i2c_buses[bus].registers[(offset+i-1)%256]=r->data[i];
            out->length=r->length;
        } else if (r->op==RYZ_PERIPHERAL_READ || r->op==RYZ_PERIPHERAL_TRANSFER) {
            if (r->op==RYZ_PERIPHERAL_TRANSFER && r->length!=1) return RYZ_PERIPHERAL_INVALID;
            unsigned offset=r->op==RYZ_PERIPHERAL_TRANSFER ? r->data[0] : 0;
            out->length=r->read_length;
            for (size_t i=0; i<out->length; ++i)
                out->data[i]=i2c_buses[bus].registers[(offset+i)%256];
        } else return RYZ_PERIPHERAL_UNSUPPORTED;
        return RYZ_PERIPHERAL_OK;
    }
    return RYZ_PERIPHERAL_CLOSED;
}
static ryz_peripheral_result_t virtual_timer(const ryz_peripheral_request_t *r,
    ryz_peripheral_reply_t *out)
{
    if (r->op == RYZ_PERIPHERAL_OPEN) {
        for (unsigned i=0; i<16; ++i) if (!timers[i].id) {
            timers[i].id=++next_id;
            timers[i].period_ms=r->config.timer.period_ms;
            timers[i].periodic=r->config.timer.periodic;
            timers[i].next_ms=clock_ms+r->config.timer.period_ms;
            out->handle=timers[i].id;
            return RYZ_PERIPHERAL_OK;
        }
        return RYZ_PERIPHERAL_NO_MEMORY;
    }
    for (unsigned i=0; i<16; ++i) if (timers[i].id==r->handle) {
        if (r->op==RYZ_PERIPHERAL_CLOSE) timers[i].id=0;
        else if (r->op==RYZ_PERIPHERAL_POLL) {
            if (clock_ms>=timers[i].next_ms) {
                out->value=timers[i].periodic ?
                    (uint32_t)((clock_ms-timers[i].next_ms)/timers[i].period_ms+1) : 1;
                if (timers[i].periodic) timers[i].next_ms+=out->value*timers[i].period_ms;
                else timers[i].next_ms=UINT64_MAX;
            }
        } else return RYZ_PERIPHERAL_UNSUPPORTED;
        return RYZ_PERIPHERAL_OK;
    }
    return RYZ_PERIPHERAL_CLOSED;
}
static ryz_peripheral_result_t virtual_pwm(const ryz_peripheral_request_t *r,
    ryz_peripheral_reply_t *out)
{
    if (r->op==RYZ_PERIPHERAL_OPEN) {
        for (unsigned i=0; i<16; ++i)
            if (pins[i].id && pins[i].pin==r->config.pwm.pin) return RYZ_PERIPHERAL_BUSY;
        for (unsigned i=0; i<4; ++i)
            if (pwm_outputs[i].id && pwm_outputs[i].pin==r->config.pwm.pin) return RYZ_PERIPHERAL_BUSY;
        for (unsigned i=0; i<4; ++i) if (!pwm_outputs[i].id) {
            pwm_outputs[i].id=++next_id;
            pwm_outputs[i].pin=r->config.pwm.pin;
            pwm_outputs[i].frequency_hz=r->config.pwm.frequency_hz;
            pwm_outputs[i].duty=r->config.pwm.duty;
            pwm_outputs[i].start_ms=clock_ms;
            out->handle=pwm_outputs[i].id;
            return RYZ_PERIPHERAL_OK;
        }
        return RYZ_PERIPHERAL_NO_MEMORY;
    }
    for (unsigned i=0; i<4; ++i) if (pwm_outputs[i].id==r->handle) {
        if (r->op==RYZ_PERIPHERAL_CLOSE) pwm_outputs[i].id=0;
        else if (r->op==RYZ_PERIPHERAL_SET_DUTY) pwm_outputs[i].duty=r->value;
        else return RYZ_PERIPHERAL_UNSUPPORTED;
        return RYZ_PERIPHERAL_OK;
    }
    return RYZ_PERIPHERAL_CLOSED;
}
static ryz_peripheral_result_t virtual_serial(const ryz_peripheral_request_t *r,
    ryz_peripheral_reply_t *out)
{
    if (r->op == RYZ_PERIPHERAL_OPEN) {
        int port = r->config.uart.port - 1;
        if (port < 0 || port >= 2 || (r->config.uart.tx < 0 && r->config.uart.rx < 0))
            return RYZ_PERIPHERAL_INVALID;
        if (serial_ports[port].id) return RYZ_PERIPHERAL_BUSY;
        serial_ports[port].id = ++next_id;
        serial_ports[port].tx = r->config.uart.tx;
        serial_ports[port].rx = r->config.uart.rx;
        serial_ports[port].length = 4;
        serial_ports[port].error_events = 3;
        memcpy(serial_ports[port].bytes, "A\0B\xff", 4);
        out->handle = serial_ports[port].id;
        return RYZ_PERIPHERAL_OK;
    }
    for (unsigned port = 0; port < 2; ++port) if (serial_ports[port].id == r->handle) {
        if (r->op == RYZ_PERIPHERAL_CLOSE) {
            serial_ports[port].id = 0;
        } else if (r->op == RYZ_PERIPHERAL_READ) {
            if (serial_ports[port].rx < 0) return RYZ_PERIPHERAL_UNSUPPORTED;
            out->length = r->read_length < serial_ports[port].length ?
                r->read_length : serial_ports[port].length;
            memcpy(out->data, serial_ports[port].bytes, out->length);
            serial_ports[port].length -= out->length;
            memmove(serial_ports[port].bytes, serial_ports[port].bytes + out->length,
                serial_ports[port].length);
            out->error_events = serial_ports[port].error_events;
            out->loss_possible = true;
        } else if (r->op == RYZ_PERIPHERAL_WRITE) {
            if (serial_ports[port].tx < 0) return RYZ_PERIPHERAL_UNSUPPORTED;
            /* The peer accepts at most three bytes per non-blocking write. */
            out->length = r->length < 3 ? r->length : 3;
            size_t room = sizeof(serial_ports[port].bytes) - serial_ports[port].length;
            if (out->length > room) out->length = room;
            memcpy(serial_ports[port].bytes + serial_ports[port].length, r->data, out->length);
            serial_ports[port].length += out->length;
        } else return RYZ_PERIPHERAL_UNSUPPORTED;
        return RYZ_PERIPHERAL_OK;
    }
    return RYZ_PERIPHERAL_CLOSED;
}
static ryz_peripheral_result_t virtual_call(void *ctx, const ryz_peripheral_request_t *r,
    ryz_peripheral_reply_t *out)
{
    (void)ctx; memset(out,0,sizeof(*out));
    if(r->kind==RYZ_PERIPHERAL_UART) return virtual_serial(r,out);
    if(r->kind==RYZ_PERIPHERAL_TIMER) return virtual_timer(r,out);
    if(r->kind==RYZ_PERIPHERAL_PWM) return virtual_pwm(r,out);
    if(r->kind==RYZ_PERIPHERAL_I2C) return virtual_i2c(r,out);
    if(r->kind==RYZ_PERIPHERAL_SPI) return virtual_spi(r,out);
    if(r->kind==RYZ_PERIPHERAL_ADC) return virtual_adc(r,out);
    if(r->kind==RYZ_PERIPHERAL_LOG) return virtual_log(r,out);
    if(r->kind==RYZ_PERIPHERAL_LED) return virtual_led(r,out);
    if(r->kind!=RYZ_PERIPHERAL_GPIO) return RYZ_PERIPHERAL_UNAVAILABLE;
    if(r->op==RYZ_PERIPHERAL_OPEN) {
        if(spi_peer.id && (spi_peer.sclk==r->config.gpio.pin || spi_peer.mosi==r->config.gpio.pin ||
            spi_peer.miso==r->config.gpio.pin || spi_peer.cs==r->config.gpio.pin)) return RYZ_PERIPHERAL_BUSY;
        for(unsigned i=0;i<6;++i) if(analog_inputs[i].id && analog_inputs[i].pin==r->config.gpio.pin)
            return RYZ_PERIPHERAL_BUSY;
        if(i2c_buses[1].id && (i2c_buses[1].sda==r->config.gpio.pin ||
            i2c_buses[1].scl==r->config.gpio.pin)) return RYZ_PERIPHERAL_BUSY;
        for(unsigned i=0;i<4;++i) if(pwm_outputs[i].id && pwm_outputs[i].pin==r->config.gpio.pin) return RYZ_PERIPHERAL_BUSY;
        for(unsigned i=0;i<16;++i) if(pins[i].id && pins[i].pin==r->config.gpio.pin) return RYZ_PERIPHERAL_BUSY;
        for(unsigned i=0;i<16;++i) if(!pins[i].id) {
            pins[i].id=++next_id; pins[i].pin=r->config.gpio.pin; pins[i].level=r->config.gpio.initial;
            out->handle=pins[i].id; return RYZ_PERIPHERAL_OK;
        }
        return RYZ_PERIPHERAL_NO_MEMORY;
    }
    for(unsigned i=0;i<16;++i) if(pins[i].id==r->handle) {
        if(r->op==RYZ_PERIPHERAL_CLOSE) pins[i].id=0;
        else if(r->op==RYZ_PERIPHERAL_WRITE) { pins[i].level=r->value; stopped=stop_after_write; }
        else if(r->op==RYZ_PERIPHERAL_READ) {
            out->value=pins[i].level;
            for(unsigned p=0;p<4;++p) if(pins[i].pin==18 && pwm_outputs[p].id && pwm_outputs[p].pin==13) {
                uint64_t phase=((clock_ms-pwm_outputs[p].start_ms)*pwm_outputs[p].frequency_hz)%1000;
                out->value=phase<pwm_outputs[p].duty ? 1 : 0;
            }
        }
        else return RYZ_PERIPHERAL_UNSUPPORTED;
        return RYZ_PERIPHERAL_OK;
    }
    return RYZ_PERIPHERAL_CLOSED;
}
static bool cancel(void *ctx) { (void)ctx; return stopped; }
static uint64_t now(void *ctx) { (void)ctx; return clock_ms; }
static void pause_ms(void *ctx, unsigned ms) { (void)ctx; clock_ms += ms ? ms : 1; }
static void *resize(void *p, size_t n)
{ if (!n) { free(p); return NULL; } return realloc(p, n); }
static void run_result(const char *body, const char *phase, const char *error)
{
    char source[8192]; snprintf(source, sizeof(source), "-- ryz-app/1\n%s", body);
    stopped=false;
    ryz_app_platform_t platform = {.resize=resize, .now_ms=now, .pause_ms=pause_ms,
        .cancelled=cancel,.peripheral_call=virtual_devices ? virtual_call : NULL};
    ryz_lua_result_t result;
    ryz_app_execute(source, strlen(source), "@peripheral-test.lua", 100, &platform, &result);
    if (strcmp(result.phase, phase)) fprintf(stderr, "%s: %s\n%s\n", result.phase, result.error, body);
    assert(!strcmp(result.phase, phase));
    if (error && !strstr(result.error, error))
        fprintf(stderr, "expected error containing '%s', got '%s'\n%s\n", error, result.error, body);
    assert(!error || strstr(result.error, error));
}
static void run(const char *body, const char *phase) { run_result(body,phase,NULL); }
static void run_error(const char *body, const char *error) { run_result(body,"runtime",error); }
int main(void)
{
    run("local g=require('gpio'); local p,e=g.open{pin=13,mode='input'}; "
        "assert(p==nil and e=='unavailable')", "done");
    virtual_devices=true;
    run("local g=require('gpio'); local p=assert(g.open{pin=13,mode='output',initial=1}); "
        "assert(p:read()==1); assert(p:write(0)); assert(p:read()==0); "
        "local x,e=g.open{pin=13}; assert(x==nil and e=='busy'); "
        "assert(p:close()); local q=assert(g.open{pin=13,mode='output'}); "
        "local v,e=p:read(); assert(v==nil and e=='closed'); assert(p:close()); "
        "assert(q:write(1)); assert(q:read()==1)", "done");
    /* A new user job can acquire the previous job's unclosed resource. */
    run("assert(require('gpio').open{pin=13})", "done");
    run("local p=assert(require('gpio').open{pin=13}); error('expected')", "runtime");
    run("assert(require('gpio').open{pin=13})", "done");
    run("require('gpio').open{pin=13,mode='outpt'}", "runtime");
    run("require('gpio').open{pin=13,frequncy=50}", "runtime");
    run("require('gpio').open{pin='13'}", "runtime");
    run("require('gpio\\0x')", "runtime");
    stop_after_write=true;
    run("local p=assert(require('gpio').open{pin=13,mode='output'}); "
        "local co=coroutine.create(function() p:write(1) end); coroutine.resume(co); "
        "error('cancel was swallowed')", "stopped");
    stop_after_write=false;
    run("assert(require('gpio').open{pin=13})", "done");
    virtual_devices=false;
    run("local p,e=require('uart').open{rx=17}; assert(p==nil and e=='unavailable')", "done");
    virtual_devices=true;
    run("local uart=require('uart'); "
        "local p=assert(uart.open{port=1,tx=16,rx=17,baud=115200,bits=8,parity='none',stop=1}); "
        "local bytes,status=p:read(2,0); assert(bytes=='A\\0' and type(status)=='table'); "
        "assert(status.error_events=='3' and status.loss_possible==true and status.dropped_bytes==nil); "
        "bytes,status=p:read(4,20); assert(bytes=='B'..string.char(255) and status.error_events=='3'); "
        "bytes,status=p:read(); assert(bytes=='' and status.error_events=='3' and status.loss_possible); "
        "assert(p:write('X\\0YZ')==3); assert(p:read(256,0)=='X\\0Y'); "
        "local other,e=uart.open{port=1,rx=18}; assert(other==nil and e=='busy'); "
        "assert(p:close()); assert(p:close()); "
        "local value,closed=p:read(); assert(value==nil and closed=='closed'); "
        "value,closed=p:write('x'); assert(value==nil and closed=='closed'); "
        "assert(uart.open{port=1,rx=17})", "done");
    /* Normal exit and a script error must both release an unclosed port. */
    run("assert(require('uart').open{rx=17})", "done");
    virtual_devices=false;
    run("local p,e=require('timer').open{period_ms=10}; assert(p==nil and e=='unavailable')", "done");
    virtual_devices=true;
    run("local timer=require('timer'); local board=require('ryzobee'); "
        "local t=assert(timer.open{period_ms=10}); assert(t:poll()==0); "
        "board.sleep_ms(9); assert(t:poll()==0); "
        "board.sleep_ms(1); assert(t:poll()==1); assert(t:poll()==0); "
        "board.sleep_ms(35); assert(t:poll()==3); assert(t:poll()==0); "
        "board.sleep_ms(5); assert(t:poll()==1); "
        "assert(t:close()); assert(t:close()); "
        "local n,e=t:poll(); assert(n==nil and e=='closed')", "done");
    run("local t=assert(require('timer').open{period_ms=5,periodic=false}); "
        "local board=require('ryzobee'); assert(t:poll()==0); "
        "board.sleep_ms(25); assert(t:poll()==1); assert(t:poll()==0); "
        "board.sleep_ms(20); assert(t:poll()==0)", "done");
    run_error("local t=assert(require('timer').open{period_ms=1}); error('TIMER_JOB_ABORT')", "TIMER_JOB_ABORT");
    run("local ts={}; for i=1,16 do ts[i]=assert(require('timer').open{period_ms=1}) end", "done");
    run_error("require('timer').open{period_ms=0}", "period_ms");
    run_error("require('timer').open{period_ms=3600001}", "period_ms");
    run_error("require('timer').open{period_ms=10,periodic=1}", "periodic");
    run_error("require('timer').open{period_ms=10,periodc=false}", "periodc");
    virtual_devices=false;
    run("local p,e=require('pwm').open{pin=13,frequency_hz=100,duty=500}; "
        "assert(p==nil and e=='unavailable')", "done");
    virtual_devices=true;
    run("local pwm=require('pwm'); local gpio=require('gpio'); local board=require('ryzobee'); "
        "local input=assert(gpio.open{pin=18,mode='input'}); "
        "local p=assert(pwm.open{pin=13,frequency_hz=100,duty=500}); "
        "assert(input:read()==1); assert(p:set_duty(0)); assert(input:read()==0); "
        "assert(p:set_duty(1000)); assert(input:read()==1); "
        "assert(p:set_duty(500)); board.sleep_ms(6); assert(input:read()==0); "
        "board.sleep_ms(4); assert(input:read()==1); "
        "local other,e=gpio.open{pin=13}; assert(other==nil and e=='busy'); "
        "assert(p:close()); assert(p:close()); "
        "local ok,closed=p:set_duty(0); assert(ok==nil and closed=='closed'); "
        "assert(gpio.open{pin=13})", "done");
    run_error("local p=assert(require('pwm').open{pin=13,frequency_hz=100,duty=0}); error('PWM_JOB_ABORT')", "PWM_JOB_ABORT");
    run("local p=assert(require('pwm').open{pin=13,frequency_hz=100,duty=1000}); assert(p:set_duty(0))", "done");
    run("assert(require('gpio').open{pin=13})", "done");
    run_error("require('pwm').open{pin=13,frequency_hz=0,duty=0}", "frequency_hz");
    run_error("require('pwm').open{pin=13,frequency_hz=1000001,duty=0}", "frequency_hz");
    run_error("require('pwm').open{pin=13,frequency_hz=100,duty=1001}", "duty");
    run_error("local p=assert(require('pwm').open{pin=13,frequency_hz=100,duty=0}); p:set_duty(-1)", "range");
    run_error("local p=assert(require('pwm').open{pin=13,frequency_hz=100,duty=0}); p:set_duty(1001)", "range");
    run_error("local p=assert(require('pwm').open{pin=13,frequency_hz=100,duty=0}); p:set_duty('500')", "integer");
    run("assert(require('gpio').open{pin=13})", "done");
    run_error("local p=assert(require('uart').open{rx=17}); error('UART_JOB_ABORT')", "UART_JOB_ABORT");
    run("local p=assert(require('uart').open{rx=17}); "
        "local n,e=p:write('x'); assert(n==nil and e=='unsupported')", "done");
    run_error("require('uart').open{rx=17,baudd=115200}", "baudd");
    run_error("require('uart').open{rx=17,baud='115200'}", "baud");
    run_error("require('uart').open{rx=17,port=0}", "port");
    run_error("require('uart').open{rx=17,baud=299}", "baud");
    run_error("require('uart').open{rx=17,bits=9}", "bits");
    run_error("require('uart').open{rx=17,parity='NONE'}", "parity");
    run_error("require('uart').open{rx=17,stop=3}", "stop");
    run_error("local p=assert(require('uart').open{rx=17}); p:read(257,0)", "range");
    run_error("local p=assert(require('uart').open{rx=17}); p:read(1,21)", "range");
    run_error("local p=assert(require('uart').open{tx=16}); p:write(string.rep('x',257))", "256");
    run_error("local p=assert(require('uart').open{tx=16}); p:write(123)", "string");
    run_error("local p=assert(require('uart').open{tx=16}); p:write('')", "256");
    run_error("local p=assert(require('gpio').open{pin=13}); p:close(1)", "arguments");
    run("assert(require('uart').open{rx=17})", "done");
    virtual_devices=false;
    run("local p,e=require('i2c').open{sda=13,scl=14}; assert(p==nil and e=='unavailable')", "done");
    virtual_devices=true;
    run("local bus=assert(require('i2c').open{sda=13,scl=14,frequency_hz=400000}); "
        "assert(bus:probe(0x50,10)==true); "
        "local found,reason=bus:probe(0x51,10); assert(found==false); "
        "found,reason=bus:probe(0x52,10); assert(found==nil and reason=='timeout'); "
        "assert(bus:transfer(0x50,string.char(2),3,10)=='\\0Z'..string.char(255)); "
        "assert(bus:write(0x50,string.char(4,1,0,255),10)==4); "
        "assert(bus:read(0x50,7,10)=='RY\\0Z'..string.char(1,0,255)); "
        "local bytes,err=bus:read(0x51,1); assert(bytes==nil and err=='not_found'); "
        "bytes,err=bus:transfer(0x52,string.char(0),1,20); assert(bytes==nil and err=='timeout'); "
        "local pin,busy=require('gpio').open{pin=13}; assert(pin==nil and busy=='busy'); "
        "assert(bus:close()); assert(bus:close()); "
        "local value,closed=bus:probe(0x50); assert(value==nil and closed=='closed'); "
        "assert(require('gpio').open{pin=13})", "done");
    run("local bus=assert(require('i2c').open{board=true}); assert(bus:probe(0x15,10)==true); "
        "assert(bus:probe(0x50,10)==false); "
        "local value,err=bus:read(0x15,1,10); assert(value==nil and err=='unsupported'); "
        "value,err=bus:write(0x15,'x',10); assert(value==nil and err=='unsupported'); "
        "value,err=bus:transfer(0x15,'x',1,10); assert(value==nil and err=='unsupported')", "done");
    run_error("local bus=assert(require('i2c').open{sda=13,scl=14}); error('I2C_JOB_ABORT')", "I2C_JOB_ABORT");
    run("assert(require('i2c').open{sda=13,scl=14})", "done");
    run_error("require('i2c').open{sda=13,scl=13}", "differ");
    run_error("require('i2c').open{sda=13}", "scl");
    run_error("require('i2c').open{sda=13,scl=14,sdc=15}", "sdc");
    run_error("require('i2c').open{sda=13,scl=14,frequency_hz=200000}", "frequency_hz");
    run_error("require('i2c').open{board=true,sda=13}", "board");
    run_error("require('i2c').open{board=true,frequency_hz=400000}", "board");
    run_error("local b=assert(require('i2c').open{sda=13,scl=14}); b:probe(7)", "range");
    run_error("local b=assert(require('i2c').open{sda=13,scl=14}); b:probe(0x78)", "range");
    run_error("local b=assert(require('i2c').open{sda=13,scl=14}); b:probe('80')", "integer");
    run_error("local b=assert(require('i2c').open{sda=13,scl=14}); b:read(0x50,257)", "range");
    run_error("local b=assert(require('i2c').open{sda=13,scl=14}); b:write(0x50,string.rep('x',257))", "256");
    run_error("local b=assert(require('i2c').open{sda=13,scl=14}); b:transfer(0x50,'x',1,21)", "range");
    run("assert(require('i2c').open{sda=13,scl=14})", "done");
    virtual_devices=false;
    run("local p,e=require('spi').open{sclk=13,mosi=14,miso=15,cs=16}; "
        "assert(p==nil and e=='unavailable')", "done");
    virtual_devices=true;
    run("local spi=require('spi'); local b=assert(spi.open{sclk=13,mosi=14,miso=15,cs=16,frequency_hz=1000000,mode=3}); "
        "assert(b:transfer(string.char(0,128,255,65))==string.char(255,127,0,190)); "
        "assert(b:read(2,0)=='SP'); assert(b:read(2,20)=='\\0I'); "
        "assert(b:write('A\\0BCD')==3); assert(b:read(3)=='A\\0B'); "
        "local p,e=require('gpio').open{pin=13}; assert(p==nil and e=='busy'); "
        "p,e=spi.open{sclk=17,mosi=18,cs=21}; assert(p==nil and e=='busy'); "
        "assert(b:close()); assert(b:close()); "
        "local value,closed=b:transfer('x'); assert(value==nil and closed=='closed'); "
        "assert(require('gpio').open{pin=13})", "done");
    run("local b=assert(require('spi').open{sclk=13,mosi=14,cs=16}); "
        "assert(b:write('OK')==2); local v,e=b:read(1); assert(v==nil and e=='unsupported')", "done");
    run("local b=assert(require('spi').open{sclk=13,miso=15,cs=16}); "
        "assert(b:read(4)=='SP\\0I'); local v,e=b:write('x'); assert(v==nil and e=='unsupported')", "done");
    run_error("local b=assert(require('spi').open{sclk=13,mosi=14,cs=16}); error('SPI_JOB_ABORT')", "SPI_JOB_ABORT");
    run("assert(require('spi').open{sclk=13,mosi=14,cs=16})", "done");
    run_error("require('spi').open{sclk=13,mosi=14,cs=16,mode=4}", "mode");
    run_error("require('spi').open{sclk=13,mosi=14,cs=16,frequency_hz=99999}", "frequency_hz");
    run_error("require('spi').open{sclk=13,mosi=14,cs=16,frequency_hz=20000001}", "frequency_hz");
    run_error("require('spi').open{sclk=13,mosi=13,cs=16}", "differ");
    run_error("require('spi').open{sclk=13,mosi=14,cs=16,mossi=17}", "mossi");
    run_error("require('spi').open{mosi=14,cs=16}", "sclk");
    run_error("require('spi').open{sclk=13,mosi=14}", "cs");
    run_error("local b=assert(require('spi').open{sclk=13,mosi=14,miso=15,cs=16}); b:transfer(string.rep('x',257))", "256");
    run_error("local b=assert(require('spi').open{sclk=13,mosi=14,miso=15,cs=16}); b:read(257)", "range");
    run_error("local b=assert(require('spi').open{sclk=13,mosi=14,miso=15,cs=16}); b:read(1,21)", "range");
    run_error("local b=assert(require('spi').open{sclk=13,mosi=14,cs=16}); b:write(123)", "string");
    run("assert(require('gpio').open{pin=13})", "done");
    virtual_devices=false;
    run("local p,e=require('adc').open{pin=13}; assert(p==nil and e=='unavailable')", "done");
    virtual_devices=true;
    run("local adc=require('adc'); local a=assert(adc.open{pin=13,attenuation_db=12}); "
        "assert(a:read()==2048); assert(a:read_mv()==1650); "
        "local p,e=adc.open{pin=13}; assert(p==nil and e=='busy'); "
        "p,e=require('gpio').open{pin=13}; assert(p==nil and e=='busy'); "
        "assert(a:close()); assert(a:close()); "
        "local v,closed=a:read(); assert(v==nil and closed=='closed'); "
        "v,closed=a:read_mv(); assert(v==nil and closed=='closed'); "
        "assert(require('gpio').open{pin=13})", "done");
    run("local a=assert(require('adc').open{pin=14,attenuation_db=2}); "
        "assert(a:read()==1024); local mv,e=a:read_mv(); assert(mv==nil and e=='unsupported')", "done");
    run("local a=assert(require('adc').open{pin=15}); "
        "local raw,e=a:read(); assert(raw==nil and e=='timeout'); "
        "local mv,e=a:read_mv(); assert(mv==nil and e=='timeout')", "done");
    run_error("local a=assert(require('adc').open{pin=13}); error('ADC_JOB_ABORT')", "ADC_JOB_ABORT");
    run("assert(require('adc').open{pin=13})", "done");
    run_error("require('adc').open{pin=13,attenuation_db=3}", "attenuation_db");
    run_error("require('adc').open{pin=13,attenuation_db='12'}", "attenuation_db");
    run_error("require('adc').open{pin=13,attenuatio_db=12}", "attenuatio_db");
    run_error("require('adc').open{}", "pin");
    run_error("local a=assert(require('adc').open{pin=13}); a:read(1)", "arguments");
    run("assert(require('gpio').open{pin=13})", "done");
    virtual_devices=false;
    run("local l=require('log'); local s,e=l.open{}; assert(s==nil and e=='unavailable'); "
        "local ok,e=l.write('info','hello'); assert(ok==nil and e=='unavailable')", "done");
    virtual_devices=true;
    run("local l=require('log'); local s=assert(l.open{}); local errors=assert(l.open{level='error'}); "
        "assert(l.write('info','A\\0B')); assert(l.write('debug','hidden')); assert(l.write('error','bad')); "
        "local b,status=s:read(3); assert(b=='I:A' and status.dropped_bytes=='0' and not status.loss_possible); "
        "assert(status.error_events==nil); assert(s:read()=='\\0B\\nE:bad\\n'); "
        "assert(errors:read()=='E:bad\\n'); assert(s:read(1,20)==''); "
        "assert(s:close()); assert(s:close()); local v,e=s:read(); assert(v==nil and e=='closed')", "done");
    run("local l=require('log'); local s=assert(l.open{level='verbose'}); "
        "assert(l.write('warn','w')); assert(l.write('verbose','v')); assert(l.write('debug','d')); "
        "assert(s:read()=='W:w\\nV:v\\nD:d\\n'); "
        "for i=1,5 do assert(l.write('info',string.rep('x',256))) end; "
        "local b,status=s:read(); assert(#b==256 and status.dropped_bytes=='518')", "done");
    run_error("require('log').open{level='warning'}", "level");
    run_error("require('log').open{level=3}", "level");
    run_error("require('log').open{levell='info'}", "levell");
    run_error("require('log').write('info')", "write");
    run_error("require('log').write(3,'message')", "level");
    run_error("require('log').write('warn','')", "256");
    run_error("require('log').write('info',string.rep('x',257))", "256");
    run_error("require('log').write('info',12)", "string");
    run_error("local s=assert(require('log').open{}); s:read(0)", "range");
    run_error("local s=assert(require('log').open{}); s:read(10,21)", "range");
    run_error("local s=assert(require('log').open{}); error('LOG_JOB_ABORT')", "LOG_JOB_ABORT");
    stop_after_write=true;
    run("local l=require('log'); assert(l.open{}); local co=coroutine.create(function() "
        "l.write('info','cancel') end); coroutine.resume(co); error('cancel was swallowed')", "stopped");
    stop_after_write=false;
    run("local l=require('log'); for i=1,16 do assert(l.open{}) end; "
        "local s,e=l.open{}; assert(s==nil and e=='no_memory')", "done");
    run("local l=require('log'); for i=1,16 do assert(l.open{}) end", "done");
    virtual_devices=false;
    run("local s,e=require('led').open{board=true}; assert(s==nil and e=='unavailable')", "done");
    virtual_devices=true; led_close_busy=true;
    run("local l=require('led'); local s=assert(l.open{board=true}); "
        "local state=s:status(); assert(state.state=='idle' and not state.output_known); "
        "assert(s:write(255,106,0)); assert(s:status().state=='pending'); "
        "local ok,e=s:write(0,0,0); assert(ok==nil and e=='busy'); "
        "require('ryzobee').sleep_ms(20); state=s:status(); "
        "assert(state.state=='ready' and state.output_known and state.red==255 and state.green==106 and state.blue==0); "
        "local other,e=l.open{board=true}; assert(other==nil and e=='busy'); "
        "ok,e=s:close(); assert(ok==nil and e=='busy'); state,e=s:status(); assert(state==nil and e=='closed'); "
        "ok,e=s:write(0,0,0); assert(ok==nil and e=='closed'); "
        "assert(s:close()); assert(s:close()); state,e=s:status(); assert(state==nil and e=='closed'); "
        "local next=assert(l.open{board=true}); assert(not next:status().output_known)", "done");
    led_fail=true;
    run("local s=assert(require('led').open{board=true}); assert(s:write(0,255,0)); "
        "require('ryzobee').sleep_ms(20); local v=s:status(); assert(v.state=='failed' and not v.output_known)", "done");
    led_fail=false;
    run_error("require('led').open{board=false}", "board");
    run_error("require('led').open{board=true,pin=45}", "pin");
    run_error("local s=assert(require('led').open{board=true}); s:write(1,2,256)", "range");
    run_error("local s=assert(require('led').open{board=true}); s:write('1',2,3)", "integer");
    run_error("local s=assert(require('led').open{board=true}); s:write(1,2)", "write");
    run_error("local s=assert(require('led').open{board=true}); s:status(1)", "arguments");
    run_error("local s=assert(require('led').open{board=true}); error('LED_JOB_ABORT')", "LED_JOB_ABORT");
    stop_after_write=true;
    run("local s=assert(require('led').open{board=true}); coroutine.resume(coroutine.create(function() "
        "s:write(0,0,0) end)); error('cancel swallowed')", "stopped");
    stop_after_write=false;
    run("assert(require('led').open{board=true})", "done");
    puts("LUA_PERIPHERALS_PASS");
}
