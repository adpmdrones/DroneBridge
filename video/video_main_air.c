/*   (c) 2015 befinitiv
 *   modified 2017 by Rodizio to work with EZ-Wifibroadcast
 *   modified 2018 by Wolfgang Christl (integration into DroneBridge https://github.com/DroneBridge)
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 2.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with this program; if not, write to the Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>
#include <net/if.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <termios.h>
#include <stdint.h>
#include <getopt.h>
#include <stdbool.h>
#include <signal.h>
#include "fec.h"
#include "video_lib.h"
#include "recorder.h"
#include "../common/db_protocol.h"
#include "../common/db_raw_send_receive.h"
#include "../common/shared_memory.h"
#include "../common/db_common.h"

#define MAX_PACKET_LENGTH (DATA_UNI_LENGTH + RADIOTAP_LENGTH + DB_RAW_V2_HEADER_LENGTH)
#define MAX_DATA_OR_FEC_PACKETS_PER_BLOCK 32
#define MAX_USER_PACKET_LENGTH 1450

bool keeprunning = true;
uint8_t comm_id, frame_type, db_vid_seqnum = 0;
unsigned int num_interfaces = 0, num_data_block = 8, num_fec_block = 4, pack_size = 1024, bitrate_op = 11, vid_adhere_80211;
db_uav_status_t *db_uav_status;
char adapters[DB_MAX_ADAPTERS][IFNAMSIZ];
db_socket_t raw_sockets[DB_MAX_ADAPTERS];
struct timespec start_time, end_time;

volatile int recorder_running = 1;
volatile uint32_t receive_count = 0;
uint8_t rec_buff[REC_BUFF_SIZE] = {0};

typedef struct {
    uint32_t seq_nr;
    int fd;
    int curr_pb;
    packet_buffer_t *pb_list;
} input_t;

static int TimeSpecToUSeconds(struct timespec *ts) {
    return (int) (ts->tv_sec + ts->tv_nsec / 1000.0);
}

void int_handler(int dummy) {
    keeprunning = false;
}

/**
 * Sends a DATA or FEC block or any other data using all available adapters
 *
 * @param seq_nr Video header sequence number
 * @param data_uni_to_ground Pointer to transmission buffer
 * @param packet_data Packet payload (FEC block or DATA block + length field)
 * @param data_length payload length
 * @param best_adapter Index of best wifi adapter inside raw_sockets[]
 */
void transmit_packet(uint32_t seq_nr, const uint8_t *packet_data, uint data_length, int best_adapter) {
    // create pointer directly to sockets send buffer (use of DB high performance send function)
    struct data_uni *data_to_ground = get_hp_raw_buffer(vid_adhere_80211);
    // set video packet to payload field of raw protocol buffer
    db_video_packet_t *db_video_p = (db_video_packet_t *) (data_to_ground->bytes);
    db_video_p->video_packet_header.sequence_number = seq_nr;
    db_uav_status->injected_packet_cnt++;

    //copy data to raw packet payload buffer (into video packet struct)
    memcpy(&db_video_p->video_packet_data, packet_data, (size_t) data_length);
    uint16_t payload_length = sizeof(video_packet_header_t) + data_length;
    if (best_adapter == 5) {
        for (int i = 0; i < num_interfaces; i++) {
            clock_gettime(CLOCK_MONOTONIC, &start_time);
            if (db_send_hp_div(&raw_sockets[i], DB_PORT_VIDEO, payload_length, update_seq_num(&db_vid_seqnum)) ==
                -1)
                db_uav_status->injection_fail_cnt++;
            clock_gettime(CLOCK_MONOTONIC, &end_time);
        }
    } else {
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        if (db_send_hp_div(&raw_sockets[best_adapter], DB_PORT_VIDEO, payload_length,
                           update_seq_num(&db_vid_seqnum)) == -1)
            db_uav_status->injection_fail_cnt++;
        clock_gettime(CLOCK_MONOTONIC, &end_time);
    }
    db_uav_status->injection_time_packet = TimeSpecToUSeconds(&end_time) - TimeSpecToUSeconds(&start_time);
}

/**
 * Takes payload data (a block), generates FEC block for DATA and sends DATA and FEC packets interleaved
 *
 * @param pbl Array where the future payload data is located as blocks of data (payload is split into arrays)
 * @param seq_nr: video_packet_header_t sequence number
 * @param fec_packet_size: FEC block size
 */
void transmit_block(packet_buffer_t *pbl, uint32_t *seq_nr, uint fec_packet_size) {
    int i;
    uint8_t *data_blocks[MAX_DATA_OR_FEC_PACKETS_PER_BLOCK];
    uint8_t fec_pool[MAX_DATA_OR_FEC_PACKETS_PER_BLOCK][MAX_USER_PACKET_LENGTH];
    uint8_t *fec_blocks[MAX_DATA_OR_FEC_PACKETS_PER_BLOCK];

    for (i = 0; i < num_data_block; ++i) {
        data_blocks[i] = pbl[i].data;
    }

    if (num_fec_block) { // Number of FEC packets per block can be 0
        for (i = 0; i < num_fec_block; ++i) {
            fec_blocks[i] = fec_pool[i];
        }
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        fec_encode(fec_packet_size, data_blocks, num_data_block, (unsigned char **) fec_blocks, num_fec_block);
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        db_uav_status->encoding_time = TimeSpecToUSeconds(&end_time) - TimeSpecToUSeconds(&start_time);
    }

    //send data and FEC packets interleaved - that algo needs to match with receiving side
    int di = 0;
    int fi = 0;
    uint32_t seq_nr_tmp = *seq_nr;
    while (di < num_data_block || fi < num_fec_block) {
        if (di < num_data_block) {
            transmit_packet(seq_nr_tmp, data_blocks[di], fec_packet_size, 5);
            seq_nr_tmp++; // every packet gets a sequence number
            di++;
        }

        if (fi < num_fec_block) {
            transmit_packet(seq_nr_tmp, fec_blocks[fi], fec_packet_size, 5);
            seq_nr_tmp++; // every packet gets a sequence number
            fi++;
        }
    }
    *seq_nr += num_data_block + num_fec_block; // block sent: update sequence number

    //reset the length back
    for (i = 0; i < num_data_block; ++i) {
        pbl[i].len = 0;
    }
    db_uav_status->injected_block_cnt++;
}

void process_command_line_args(int argc, char *argv[]) {
    num_interfaces = 0, comm_id = DEFAULT_V2_COMMID, bitrate_op = 11;
    num_data_block = 8, num_fec_block = 4, pack_size = 1024, frame_type = 1, vid_adhere_80211 = 0;
    int c;
    while ((c = getopt(argc, argv, "n:c:d:r:f:b:t:a:")) != -1) {
        switch (c) {
            case 'n':
                strncpy(adapters[num_interfaces], optarg, IFNAMSIZ);
                num_interfaces++;
                break;
            case 'c':
                comm_id = (uint8_t) strtol(optarg, NULL, 10);
                break;
            case 'd':
                num_data_block = (uint8_t) strtol(optarg, NULL, 10);
                break;
            case 'r':
                num_fec_block = (uint8_t) strtol(optarg, NULL, 10);
                break;
            case 'f':
                pack_size = (unsigned int) strtol(optarg, NULL, 10);
                break;
            case 'b':
                bitrate_op = (uint8_t) strtol(optarg, NULL, 10);
            case 't':
                frame_type = (uint8_t) strtol(optarg, NULL, 10);
                break;
            case 'a':
                vid_adhere_80211 = (uint) strtol(optarg, NULL, 10);
                break;
            default:
                printf("Based of Wifibroadcast by befinitiv, based on packetspammer by Andy Green.  Licensed under GPL2\n"
                       "This tool takes a data stream via the DroneBridge long range video port and outputs it via stdout, "
                       "UDP or TCP"
                       "\nIt uses the Reed-Solomon FEC code to repair lost or damaged packets."
                       "\n\n\t-n Name of a network interface that should be used to receive the stream. Must be in monitor "
                       "mode. Multiple interfaces supported by calling this option multiple times (-n inter1 -n inter2 -n interx)"
                       "\n\t-c [communication id] Choose a number from 0-255. Same on ground station and UAV!."
                       "\n\t-d Number of data packets in a block (default 8). Needs to match with tx."
                       "\n\t-r Number of FEC packets per block (default 4). Needs to match with tx."
                       "\n\t-f Bytes per packet (default %d. max %d). This is also the FEC "
                       "block size. Needs to match with tx."
                       "\n\t-b bit rate:\tin Mbps (1|2|5|6|9|11|12|18|24|36|48|54)\n\t\t(bitrate option only "
                       "supported with Ralink chipsets)"
                       "\n\t-t [1|2] DroneBridge v2 raw protocol packet/frame type: 1=RTS, 2=DATA (CTS protection)"
                       "\n\t-a [0|1] disable/enable. Offsets the payload by some bytes so that it sits outside the "
                       "802.11 header. Set this to 1 if you are using a non DB-Rasp Kernel!\n", 1024, DATA_UNI_LENGTH);
                abort();
        }
    }
}

// Transmission scheme: 4 Data, 2 FEC packet per block
// |-------------- Block -----------------|
// |Data - FEC - Data - FEC - Data - Data |
//  1024   1024  1024   1024  1024   1024

int main(int argc, char *argv[]) {
    signal(SIGINT, int_handler);
    setpriority(PRIO_PROCESS, 0, -10);
    process_command_line_args(argc, argv);

    input_t input;
    db_uav_status = db_uav_status_memory_open();
    db_uav_status->injection_fail_cnt = 0;
    db_uav_status->skipped_fec_cnt = 0, db_uav_status->injected_block_cnt = 0,
    db_uav_status->injection_time_packet = 0, db_uav_status->wifi_adapter_cnt = num_interfaces;
    db_uav_status->injected_packet_cnt = 0;
    int param_min_packet_length = 24;

    if (num_interfaces == 0) {
        LOG_SYS_STD(LOG_ERR, "DB_VIDEO_AIR: No interface specified. Aborting\n");
        abort();
    }

    if (pack_size > DATA_UNI_LENGTH) {
        LOG_SYS_STD(LOG_ERR, "DB_VIDEO_AIR; Packet length is limited to %d bytes (you requested %d bytes)\n",
                DATA_UNI_LENGTH, pack_size);
        abort();
    }

    if (param_min_packet_length > pack_size) {
        LOG_SYS_STD(LOG_ERR,
                "DB_VIDEO_AIR; Minimum packet length is higher than maximum packet length (%d > %d)\n",
                param_min_packet_length, pack_size);
        abort();
    }

    if (num_data_block > MAX_DATA_OR_FEC_PACKETS_PER_BLOCK || num_fec_block > MAX_DATA_OR_FEC_PACKETS_PER_BLOCK) {
        LOG_SYS_STD(LOG_ERR,
                "DB_VIDEO_AIR: Data and FEC packets per block are limited to %d (you requested %d data, %d FEC)\n",
                MAX_DATA_OR_FEC_PACKETS_PER_BLOCK, num_data_block, num_fec_block);
        abort();
    }

    input.fd = STDIN_FILENO;
    input.seq_nr = 0;
    input.curr_pb = 0;
    input.pb_list = lib_alloc_packet_buffer_list(num_data_block, MAX_PACKET_LENGTH);

    //prepare the buffers with headers
    int j = 0;
    for (j = 0; j < num_data_block; ++j) {
        input.pb_list[j].len = 0;
    }

    //initialize forward error correction
    fec_init();

    // open DroneBridge raw sockets
    for (int k = 0; k < num_interfaces; ++k) {
        raw_sockets[k] = open_db_socket(adapters[k], comm_id, 'm', bitrate_op, DB_DIREC_GROUND, DB_PORT_VIDEO,
                                        frame_type);
        strncpy(db_uav_status->adapter[k].name, adapters[k], IFNAMSIZ);
    }
    LOG_SYS_STD(LOG_INFO, "DB_VIDEO_AIR: started!\n");
    while (keeprunning) {
        // get a packet buffer from list
        packet_buffer_t *pb = input.pb_list + input.curr_pb;
        // if the buffer is fresh we add a payload header
        if (pb->len == 0) {
            pb->len += sizeof(uint32_t); //make space for a length field (will be filled later)
        }
        //read the data into packet buffer (inside block)
        ssize_t inl = read(input.fd, pb->data + pb->len, pack_size - pb->len);
        if (inl < 0 || inl > pack_size - pb->len) {
            perror("DB_VIDEO_AIR: reading stdin\n");
            abort();
        }
        if (inl == 0) { // EOF
            LOG_SYS_STD(LOG_ERR, "\nDB_VIDEO_AIR: Warning: Lost connection to stdin. Please make sure that a data source is connected");
            usleep((__useconds_t) 5e5);
            continue;
        }
        pb->len += inl;

        // check if this packet is finished
        if (pb->len >= param_min_packet_length) {
            video_packet_data_t *video_p_data = (video_packet_data_t *) (pb->data);
            video_p_data->data_length = pb->len;
            // check if this block is finished
            if (input.curr_pb == num_data_block - 1) {
                // transmit entire block - consisting of packets that get sent interleaved
                // always transmit/FEC encode packets of length pack_size, even if payload (data_length) is less
                transmit_block(input.pb_list, &(input.seq_nr),
                               pack_size); // input.pb_list is video_packet_data_t[num_fec + num_data]
                if (db_uav_status->injected_block_cnt % 500 == 1) {
                    LOG_SYS_STD(LOG_INFO, "DB_VIDEO_AIR: \ttried to inject %i packets, failed %i, injection time/packet %ius, FEC encoding time %ius         \r",
                           db_uav_status->injected_packet_cnt, db_uav_status->injection_fail_cnt,
                           db_uav_status->injection_time_packet, db_uav_status->encoding_time);
                }

                input.curr_pb = 0;
            } else {
                input.curr_pb++;
            }
        }
    }

    LOG_SYS_STD(LOG_INFO, "DB_VIDEO_AIR: Terminated!\n");
    return (0);
}
