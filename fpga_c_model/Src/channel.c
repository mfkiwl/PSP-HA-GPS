#include "channel.h"

#include <string.h>
#include <stdio.h>

const uint8_t lo_sin[] = {0, 0, 1, 1};
const uint8_t lo_cos[] = {0, 1, 1, 0};

const int SVs[] = { // PRN, Navstar, taps
     1,  63,  2,  6,
     2,  56,  3,  7,
     3,  37,  4,  8,
     4,  35,  5,  9,
     5,  64,  1,  9,
     6,  36,  2, 10,
     7,  62,  1,  8,
     8,  44,  2,  9,
     9,  33,  3, 10,
    10,  38,  2,  3,
    11,  46,  3,  4,
    12,  59,  5,  6,
    13,  43,  6,  7,
    14,  49,  7,  8,
    15,  60,  8,  9,
    16,  51,  9, 10,
    17,  57,  1,  4,
    18,  50,  2,  5,
    19,  54,  3,  6,
    20,  47,  4,  7,
    21,  52,  5,  8,
    22,  53,  6,  9,
    23,  55,  1,  3,
    24,  23,  4,  6,
    25,  24,  5,  7,
    26,  26,  6,  8,
    27,  27,  7,  9,
    28,  48,  8, 10,
    29,  61,  1,  6,
    30,  39,  2,  7,
    31,  58,  3,  8,
    32,  22,  4,  9,
};

void clock_ca(ca_t *ca) {
    uint16_t g1 = ca->g1;
    uint16_t g2 = ca->g2;
    uint16_t g1_shift_in = ((g1 >> 9) ^ (g1 >> 2)) & 0x1;
    uint16_t g2_shift_in = ((g2 >> 9) ^ (g2 >> 8) ^ (g2 >> 7) ^ (g2 >> 5) ^ (g2 >> 2) ^ (g2 >> 1)) & 0x1;
    ca->g1 = (g1 << 1) | g1_shift_in;
    ca->g2 = (g2 << 1) | g2_shift_in;
}

uint8_t get_ca(ca_t *ca) {
    uint8_t ca_out = ((ca->g1 >> 9) ^ (ca->g2 >> (ca->T0-1)) ^ (ca->g2 >> (ca->T1-1))) & 0x1;
    return ca_out;
}

uint8_t is_ca_epoch(ca_t *ca) {
    return (ca->g1 & 0x3FF) == 0x3FF;
}

void init_channel(channel_t *chan, int chan_num, int sv, double lo_dop, double ca_shift) {
    chan->chan_num = chan_num;
    chan->sv = sv;
    const double ca_dop = lo_dop/1575.42e6*1023000.0;
    chan->phase_offset = (uint32_t)(fs/500 - (ca_shift*fs/1023000.0)) % ((int64_t)fs/1000);

    chan->ca_rate = (uint32_t)((1023000.0 + ca_dop)/fs*4294967296.0);
    chan->lo_rate = (uint32_t)((fc + lo_dop)/fs*4294967296.0);

    chan->ca_phase = 0;
    chan->lo_phase = 0;

    chan->ca.T0 = SVs[chan->sv*4+2];
    chan->ca.T1 = SVs[chan->sv*4+3];
    chan->ca.g1 = 0x3FF;
    chan->ca.g2 = 0x3FF;

    chan->ie = 0, chan->qe = 0, chan->ip = 0, chan->qp = 0, chan->il = 0, chan->ql = 0;
    chan->ca_e = 0, chan->ca_p = 0, chan->ca_l = 0;

    chan->ca_freq_integrator = ((uint64_t)chan->ca_rate) << 32;
    chan->lo_freq_integrator = ((uint64_t)chan->lo_rate) << 32;

    chan->ca_en = 0;
    chan->tracked_this_epoch = 0;
    chan->first_bit = 2;
    chan->wait_epoch = 4;
    chan->nav_ms = 0;
    chan->total_ms = 0;
    chan->nav_valid = 0;
    chan->nav_bit_count = 0;
    chan->last_z_count = 0;
}

void do_sample(channel_t *chan, uint8_t sample) {
    uint8_t lo_i = lo_sin[chan->lo_phase >> 30];
    uint8_t lo_q = lo_cos[chan->lo_phase >> 30];

    uint8_t die, dqe, dip, dqp, dil, dql;

    // Mixers
    die = sample^chan->ca_e^lo_i; dqe = sample^chan->ca_e^lo_q;
    dip = sample^chan->ca_p^lo_i; dqp = sample^chan->ca_p^lo_q;
    dil = sample^chan->ca_l^lo_i; dql = sample^chan->ca_l^lo_q;

    // Integrators
    chan->ie = chan->ca_en ? (chan->ie + (die ? -1:1)) : 0;
    chan->qe = chan->ca_en ? (chan->qe + (dqe ? -1:1)) : 0;
    chan->ip = chan->ca_en ? (chan->ip + (dip ? -1:1)) : 0;
    chan->qp = chan->ca_en ? (chan->qp + (dqp ? -1:1)) : 0;
    chan->il = chan->ca_en ? (chan->il + (dil ? -1:1)) : 0;
    chan->ql = chan->ca_en ? (chan->ql + (dql ? -1:1)) : 0;
}

void process_ip_to_bit(channel_t *chan) {
    if(chan->first_bit == 2)                                                    // First call
    {
        chan->first_bit = (chan->ip > 0) ? 1:0;
        return;
    } 
    else if(chan->first_bit != 3 && ((chan->ip > 0) ? 1:0 != chan->first_bit))  // First edge
    {
        chan->nav_ms = 0;
        chan->first_bit = 3;
        return;
    }
    else if(chan->nav_ms == 0)                                                  // Sample bit
    {
        uint8_t bit = (chan->ip > 0) ? 1:0;
        if(chan->nav_bit_count < 300) {
            chan->nav_buf[chan->nav_bit_count++] = bit;
        }
    }
    chan->nav_ms = (chan->nav_ms + 1) % 20;
    chan->total_ms++;
}

uint8_t check_parity(uint8_t *bits, uint8_t *p, uint8_t D29, uint8_t D30) {
    // D29 and D30 are the bits from the previous word (ugh)
    uint8_t *d = bits - 1;
    for(uint8_t i = 1; i < 25; i++) d[i] ^= D30; // Flip to correct polarity as we go
    p[0] = D29 ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[6] ^ d[10] ^ d[11] ^ d[12] ^ d[13] ^ d[14] ^ d[17] ^ d[18] ^ d[20] ^ d[23];
    p[1] = D30 ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[7] ^ d[11] ^ d[12] ^ d[13] ^ d[14] ^ d[15] ^ d[18] ^ d[19] ^ d[21] ^ d[24];
    p[2] = D29 ^ d[1] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[8] ^ d[12] ^ d[13] ^ d[14] ^ d[15] ^ d[16] ^ d[19] ^ d[20] ^ d[22];
    p[3] = D30 ^ d[2] ^ d[4] ^ d[5] ^ d[6] ^ d[8] ^ d[9] ^ d[13] ^ d[14] ^ d[15] ^ d[16] ^ d[17] ^ d[20] ^ d[21] ^ d[23];
    p[4] = D30 ^ d[1] ^ d[3] ^ d[5] ^ d[6] ^ d[7] ^ d[9] ^ d[10] ^ d[14] ^ d[15] ^ d[16] ^ d[17] ^ d[18] ^ d[21] ^ d[22] ^ d[24];
    p[5] = D29 ^ d[3] ^ d[5] ^ d[6] ^ d[8] ^ d[9] ^ d[10] ^ d[11] ^ d[13] ^ d[15] ^ d[19] ^ d[22] ^ d[23] ^ d[24];

    return (memcmp(d+25, p, 6) == 0);
}

uint8_t process_message(channel_t *chan) {
    uint8_t preamble_norm[] = {1, 0, 0, 0, 1, 0, 1, 1};
    uint8_t preamble_inv[] = {0, 1, 1, 1, 0, 1, 0, 0};
    uint8_t p[6];

    if(memcmp(chan->nav_buf, preamble_norm, 8) == 0) p[4]=p[5]=0;
    else if(memcmp(chan->nav_buf, preamble_inv, 8) == 0) p[4]=p[5]=1;
    else return 0;

    // Check 10 word parities
    for(uint8_t i = 0; i < 10; i++) {
        if(!check_parity(chan->nav_buf+i*30, p, p[4], p[5])) {
            return 0;
        }
    }

    // Verify HOW subframe ID
    uint8_t subframe_id = (chan->nav_buf[29+20] << 2) | (chan->nav_buf[29+21] << 1) | chan->nav_buf[29+22];
    if(subframe_id < 1 || subframe_id > 5) {
        return 0;
    }
    
    // Final verification is the Z count incrementing by 1
    int32_t this_z_count = 0;
    for(uint8_t i = 30; i < 30 + 17; i++) {
        this_z_count = (this_z_count << 1) | chan->nav_buf[i];
    }
    if((chan->last_z_count + 1) % 100800 == this_z_count) {
        chan->nav_valid = 1;
    } else {
        chan->nav_valid = 0;
    }
    chan->last_z_count = this_z_count;

    //printf("Subframe ID: %d\n", subframe_id);
    return 1;
}

void clock_channel(channel_t *chan, uint8_t sample) {
    uint8_t ca_full = (chan->ca_phase >> 31) & 0x1;
    uint8_t last_ca_full = ca_full;
    if(chan->ca_en) {
        chan->ca_phase += chan->ca_rate;
    }
    chan->lo_phase += chan->lo_rate;
    ca_full = (chan->ca_phase >> 31) & 0x1;
    uint8_t ca_half = (chan->ca_phase >> 30) & 0x1;

    if(chan->phase_offset) {
        chan->phase_offset--;
    } else {
        chan->ca_en = 1;
    }

    if(ca_full & !last_ca_full) {
        clock_ca(&chan->ca);
    }
    
    chan->ca_e = get_ca(&chan->ca);
    if(ca_full && !ca_half) {
        chan->ca_l = chan->ca_p;
    } else if (!ca_full && !ca_half) {
        chan->ca_p = chan->ca_e;
    }

    do_sample(chan, sample);

    if(is_ca_epoch(&chan->ca) && !chan->tracked_this_epoch) {
        int64_t power_early = chan->ie*chan->ie + chan->qe*chan->qe;
        int64_t power_late = chan->il*chan->il + chan->ql*chan->ql;
        int64_t code_phase_err = power_early - power_late;

        chan->ca_freq_integrator += code_phase_err << 11;
        int64_t new_ca_rate = chan->ca_freq_integrator + (code_phase_err << 23);
        chan->ca_rate = new_ca_rate >> 32;

        int64_t carrier_phase_err = chan->ip*chan->qp;

        chan->lo_freq_integrator += carrier_phase_err << 21;
        int64_t new_lo_rate = chan->lo_freq_integrator + (carrier_phase_err << 28);
        chan->lo_rate = new_lo_rate >> 32;

        //printf("carrier doppler from code lock: %lld\n",  (ca_freq_integrator >> 32));
        //printf("carrier doppler from code lock:    %20f\n",  (((ca_freq_integrator/4294967296.0)*fs/(4294967296.0))-1023000.0)*1575.42e6/1023000.0);
        //printf("carrier doppler from carrier lock: %20f\n",  ((lo_freq_integrator/4294967296.0)*fs/(4294967296.0))-fc);
        //printf("carrier doppler from carrier lock: %llu\n", lo_freq_integrator);

        // Process nav bits
        if(chan->wait_epoch) {
            chan->wait_epoch--;
        } else {
            process_ip_to_bit(chan);
        }
        
        if(chan->nav_bit_count >= 300) {
            if(process_message(chan)) {
                printf("Channel %d: Preamble found at time: %d!\n", chan->chan_num, chan->total_ms);
            }
            memmove(chan->nav_buf, chan->nav_buf+1, 299);
            chan->nav_bit_count--;
        }

        chan->ie = 0;
        chan->qe = 0;
        chan->ip = 0;
        chan->qp = 0;
        chan->il = 0;
        chan->ql = 0;
        chan->tracked_this_epoch = 1;

    } else if (!is_ca_epoch(&chan->ca)) {
        chan->tracked_this_epoch = 0;
    }
}