/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.h"

#ifdef HAVE_LIBPCAP

#include <errno.h>

#include "endian_magic.h"
#include "kis_ppilogfile.h"
#include "kis_ppi.h"
#include "phy_80211.h"

KisPPILogfile::KisPPILogfile(SharedLogBuilder in_builder) : 
    KisLogfile(in_builder) {

	// Default to dot11
	dlt = DLT_IEEE802_11;

	cbfilter = NULL;
	cbaux = NULL;

	dumpfile = NULL;
	dumper = NULL;
    dump_filep = NULL;

    auto packetchain = Globalreg::FetchMandatoryGlobalAs<Packetchain>("PACKETCHAIN");

    pack_comp_80211 = packetchain->RegisterPacketComponent("PHY80211");
    pack_comp_mangleframe = packetchain->RegisterPacketComponent("MANGLEDATA");
    pack_comp_radiodata = packetchain->RegisterPacketComponent("RADIODATA");
    pack_comp_gps = packetchain->RegisterPacketComponent("GPS");
    pack_comp_checksum = packetchain->RegisterPacketComponent("CHECKSUM");
    pack_comp_decap = packetchain->RegisterPacketComponent("DECAP");
    pack_comp_linkframe = packetchain->RegisterPacketComponent("LINKFRAME");
}

bool KisPPILogfile::Log_Open(std::string in_path) {
    local_locker lock(&log_mutex);

    set_int_log_path(in_path);

	dumpfile = NULL;
	dumper = NULL;
    dump_filep = NULL;

    auto packetchain =
        Globalreg::FetchMandatoryGlobalAs<Packetchain>("PACKETCHAIN");

	dumpfile = pcap_open_dead(DLT_PPI, MAX_PACKET_LEN);

	if (dumpfile == NULL) {
        _MSG_ERROR("Failed to prepare pcap/ppi dump file '{}': {}",
                in_path, kis_strerror_r(errno));
        return false;
	}

    // Open as a filepointer
    dump_filep = fopen(in_path.c_str(), "wb");
    if (dump_filep == NULL) {
        _MSG_ERROR("Failed to open pcap/ppi dump file '{}' for writing: {}",
                in_path, kis_strerror_r(errno));
        return false;
    }

    // Close it on exec
    fcntl(fileno(dump_filep), F_SETFL, fcntl(fileno(dump_filep), F_GETFL, 0) | O_CLOEXEC);

	dumper = pcap_dump_fopen(dumpfile, dump_filep);
    if (dumper == NULL) {
        _MSG_FATAL("Unable to open pcap/ppi dump file '{}': {}",
                in_path, kis_strerror_r(errno));
        Globalreg::globalreg->fatal_condition = 1;
        return false;
	}

    _MSG_INFO("Opened PPI pcap log file '{}'", in_path);

    set_int_log_open(true);

	packetchain->RegisterHandler(&KisPPILogfile::packet_handler, this, CHAINPOS_LOGGING, -100);

    return true;
}

void KisPPILogfile::Log_Close() {
    local_locker lock(&log_mutex);

    set_int_log_open(false);

    auto packetchain =
        Globalreg::FetchGlobalAs<Packetchain>("PACKETCHAIN");
    if (packetchain != NULL) 
        packetchain->RemoveHandler(&KisPPILogfile::packet_handler, CHAINPOS_LOGGING);

    // Close files
    if (dumper != NULL) {
        pcap_dump_flush(dumper);
        pcap_dump_close(dumper);
    }

    if (dumpfile != NULL) {
        pcap_close(dumpfile);
    }

	dumper = NULL;
	dumpfile = NULL;
    dump_filep = NULL;

}

KisPPILogfile::~KisPPILogfile() {
    Log_Close();
}

void KisPPILogfile::RegisterPPICallback(dumpfile_ppi_cb in_cb, void *in_aux) {
	for (unsigned int x = 0; x < ppi_cb_vec.size(); x++) {
		if (ppi_cb_vec[x].cb == in_cb && ppi_cb_vec[x].aux == in_aux)
			return;
	}

	ppi_cb_rec r;
	r.cb = in_cb;
	r.aux = in_aux;

	ppi_cb_vec.push_back(r);
}

void KisPPILogfile::RemovePPICallback(dumpfile_ppi_cb in_cb, void *in_aux) {
	for (unsigned int x = 0; x < ppi_cb_vec.size(); x++) {
		if (ppi_cb_vec[x].cb == in_cb && ppi_cb_vec[x].aux == in_aux) {
			ppi_cb_vec.erase(ppi_cb_vec.begin() + x);
			return;
		}
	}
}

int KisPPILogfile::packet_handler(CHAINCALL_PARMS) {
    KisPPILogfile *ppilog = (KisPPILogfile *) auxdata;

    local_locker lg(&(ppilog->packet_mutex));

    if (ppilog->stream_paused)
        return 1;

    // Grab the mangled frame if we have it, then try to grab up the list of
    // data types and die if we can't get anything
    dot11_packinfo *packinfo =
        (dot11_packinfo *) in_pack->fetch(ppilog->pack_comp_80211);

    // Grab the generic mangled frame
    kis_datachunk *chunk = 
        (kis_datachunk *) in_pack->fetch(ppilog->pack_comp_mangleframe);

    kis_layer1_packinfo *radioinfo =
        (kis_layer1_packinfo *) in_pack->fetch(ppilog->pack_comp_radiodata);

    kis_gps_packinfo *gpsdata =
        (kis_gps_packinfo *) in_pack->fetch(ppilog->pack_comp_gps);

    kis_packet_checksum *fcsdata =
        (kis_packet_checksum *) in_pack->fetch(ppilog->pack_comp_checksum);

    if (ppilog->cbfilter != NULL) {
        // If we have a filter, grab the data using that
        chunk = (*(ppilog->cbfilter))(in_pack, ppilog->cbaux);
    } else if (chunk == NULL) {
        // Look for the 802.11 frame
        if ((chunk = (kis_datachunk *) in_pack->fetch(ppilog->pack_comp_decap)) == NULL) {
            // Look for any link frame, we'll check the DLT soon
            chunk = (kis_datachunk *) in_pack->fetch(ppilog->pack_comp_linkframe);
        }
    }

    // If after all of that we still didn't find a packet
    if (chunk == NULL) {
        return 0;
    }

    if (chunk != NULL && chunk->length > MAX_PACKET_LEN) {
        _MSG("Weird frame in pcap logger with the wrong size...", MSGFLAG_ERROR);
        return 0;
    }

    int dump_offset = 0;

    unsigned int dump_len = 0;
    if (chunk != NULL)
        dump_len += chunk->length;

    u_char *dump_data = NULL;

    // Assemble the full packet
    ppi_packet_header *ppi_ph;
    int gps_tagsize = 0; //include struct ppi fieldheader
    int dot11common_tagsize = 0; //include ppi_fieldheader

    unsigned int ppi_len = 0;
    unsigned int ppi_pos = sizeof(ppi_packet_header);

    /* The size of the gps tag varies depending if we have altitude or not
     * we do all of the length math up front, and throughout the rest of the
     * function make our decision based on the length, not the gpsdata.
     * this helps keep logic error minimized.
     */
    if (gpsdata != NULL) {
        gps_tagsize = sizeof(ppi_gps_hdr); //12
        if (gpsdata->fix <= 1) //no fix
            gps_tagsize = 0; //dont bother storing anything
        if (gpsdata->fix >= 2) 
            gps_tagsize += 12; // lon, lat, appid, 
        if (gpsdata->fix >= 3)
            gps_tagsize +=4; // altitude
        //Could eventually include hdop, vdop using simillar scheme here
    }
    /* although dot11common tags are constant size, we follow the same pattern here*/
    if (radioinfo != NULL) {
        dot11common_tagsize = sizeof(ppi_80211_common);

        if (fcsdata != NULL)
            dump_len += 4;
    }
    //printf("sizeof ppi_gps_hdr:%d\n", sizeof(ppi_gps_hdr));
    //printf("Computed gps tagsize of %d\n", gps_tagsize);
    //printf("Computed dot11common tagsize of %d\n", dot11common_tagsize);
    //printf("Sizeof ppi_packetheader: %d\n", sizeof(ppi_packet_header));
    ppi_len += sizeof(ppi_packet_header);
    ppi_len += gps_tagsize;
    ppi_len += dot11common_tagsize;
    //printf("ppi_len=%d\n", ppi_len);

    //With the static-ppi fields out of the way, handle any dynamic ones
    //(f.ex) plugin-spectool
    // Collate the allocation sizes of any callbacks
    for (unsigned int p = 0; p < ppilog->ppi_cb_vec.size(); p++) {
        ppi_len += (*(ppilog->ppi_cb_vec[p].cb))(1, in_pack, NULL, 0, ppilog->ppi_cb_vec[p].aux);
    }

    dump_len += ppi_len; //dumplen now accounts for all ppi data

    if (dump_len == 0 || ppi_len == 0)
        return 0;

    dump_data = new u_char[dump_len];
    //memset(dump_data, 0xcc, dump_len); //Good for debugging ppi stuff.
    ppi_ph = (ppi_packet_header *) dump_data;

    ppi_ph->pph_version = 0;
    ppi_ph->pph_flags = 0;
    ppi_ph->pph_len = kis_htole16(ppi_len);

    // Use the DLT in the PPI internal
    ppi_ph->pph_dlt = kis_htole32(ppilog->dlt);

    //First lay out the GPS tag, if applicable
    if (gpsdata != NULL) {
        unsigned int gps_data_offt = 0; //offsets to fields, from begging of field data.
        if (gps_tagsize > 0) {
            ppi_gps_hdr *ppigps = NULL;
            union block {
                uint8_t u8;
                uint16_t u16;
                uint32_t u32;
            } *u;

            //printf("debug - logging ppi gps packet. gps_tagsize: %d\n", gps_tagsize);
            ppigps = (ppi_gps_hdr *) &(dump_data[ppi_pos]);
            ppigps->pfh_datatype = kis_htole16(PPI_FIELD_GPS);
            // Header + lat/lon minus PPI overhead. 
            ppigps->pfh_datalen = gps_tagsize - 4; //subtract ppi fieldheader

            ppigps->version = 2;
            ppigps->fields_present = PPI_GPS_FLAG_LAT | PPI_GPS_FLAG_LON | PPI_GPS_FLAG_APPID;

            //GPSLAT
            //printf("lat: %3.7f %f \n", gpsdata->lat, gpsdata->lat);
            u = (block *) &(ppigps->field_data[gps_data_offt]);
            u->u32 = kis_htole32(double_to_fixed3_7(gpsdata->lat));
            gps_data_offt += 4;

            //GPSLON
            //printf("lon: %3.7f %f\n", gpsdata->lon, gpsdata->lon);
            u = (block *) &(ppigps->field_data[gps_data_offt]);
            u->u32 = kis_htole32(double_to_fixed3_7(gpsdata->lon));
            gps_data_offt += 4;

            //GPSALT
            if (gps_tagsize >= 28) //include alt
            {
                u = (block *) &(ppigps->field_data[gps_data_offt]);
                u->u32 = kis_htole32(double_to_fixed6_4(gpsdata->alt));
                //u->u32 = kis_htole32(0x6b484390);
                gps_data_offt += 4;

                ppigps->fields_present |= PPI_GPS_FLAG_ALT;
            }
            //APPID
            //printf("gps_data_offt %d gpslen = %d\n", gps_data_offt,ppigps->gps_len);
            u = (block *) &(ppigps->field_data[gps_data_offt]);
            u->u32 = kis_htole32(0x0053494B); //KIS0
            gps_data_offt += 4;
            ppigps->magic = PPI_GPS_MAGIC;
            ppigps->gps_len = gps_tagsize - 4; //subtract ppi fieldheader


            // Convert endian state
            ppigps->fields_present = kis_htole32(ppigps->fields_present);
            ppigps->pfh_datalen = kis_htole32(ppigps->pfh_datalen);
            ppigps->gps_len = kis_htole16(ppigps->gps_len);
            //Advance ppi cursor for other PPI tags.
            ppi_pos += gps_tagsize;
        } //tagsize > 0

    } //gpsdata present

    dump_offset = ppi_pos;

    if (radioinfo != NULL) {
        ppi_80211_common *ppi_common;
        ppi_common = (ppi_80211_common *) &(dump_data[ppi_pos]);
        ppi_pos += sizeof(ppi_80211_common);

        ppi_common->pfh_datatype = kis_htole16(PPI_FIELD_11COMMON);
        ppi_common->pfh_datalen = kis_htole16(sizeof(ppi_80211_common) -
                sizeof(ppi_field_header));

        if (packinfo != NULL) 
            ppi_common->tsf_timer = kis_htole64(packinfo->timestamp);
        else
            ppi_common->tsf_timer = 0;

        // Assemble the flags in host mode then convert them all at once
        ppi_common->flags = 0;

        if (packinfo != NULL && packinfo->corrupt)
            ppi_common->flags |= PPI_80211_FLAG_PHYERROR;
        if (fcsdata != NULL) {
            ppi_common->flags |= PPI_80211_FLAG_FCS;

            if (fcsdata->checksum_valid == 0)
                ppi_common->flags |= PPI_80211_FLAG_INVALFCS;
        }

        ppi_common->flags = kis_htole16(ppi_common->flags);

        ppi_common->rate = kis_htole16(radioinfo->datarate / 5);
        ppi_common->freq_mhz = kis_htole16((uint16_t) (radioinfo->freq_khz / 1000));

        // Assemble the channel flags then endian swap them
        ppi_common->chan_flags = 0;
        switch (radioinfo->encoding) {
            case encoding_cck:
                ppi_common->chan_flags |= PPI_80211_CHFLAG_CCK;
                break;
            case encoding_ofdm:
                ppi_common->chan_flags |= PPI_80211_CHFLAG_OFDM;
                break;
            case encoding_dynamiccck:
                ppi_common->chan_flags |= PPI_80211_CHFLAG_DYNAMICCCK;
                break;
            case encoding_gfsk:
                ppi_common->chan_flags |= PPI_80211_CHFLAG_GFSK;
                break;
            case encoding_pbcc:
            case encoding_unknown:
                break;
        }
        switch (radioinfo->carrier) {
            case carrier_80211b:
                ppi_common->chan_flags |= (PPI_80211_CHFLAG_2GHZ | PPI_80211_CHFLAG_CCK);
                break;
            case carrier_80211bplus:
                ppi_common->chan_flags |= (PPI_80211_CHFLAG_2GHZ | PPI_80211_CHFLAG_CCK | PPI_80211_CHFLAG_TURBO);
                break;
            case carrier_80211a:
                ppi_common->chan_flags |= (PPI_80211_CHFLAG_5GHZ | PPI_80211_CHFLAG_OFDM);
                break;
            case carrier_80211g:
                // Could be PPI_80211_CHFLAG_OFDM or PPI_80211_CHFLAG_DYNAMICCCK
                ppi_common->chan_flags |= PPI_80211_CHFLAG_2GHZ;
                break;
            case carrier_80211fhss:
                ppi_common->chan_flags |= (PPI_80211_CHFLAG_2GHZ | PPI_80211_CHFLAG_GFSK);
                break;
            case carrier_80211dsss:
                ppi_common->chan_flags |= PPI_80211_CHFLAG_2GHZ;
                break;
            case carrier_80211n20:
            case carrier_80211n40:
                // FIXME Dunno how to restore spectrum
                ppi_common->chan_flags |= PPI_80211_CHFLAG_OFDM;
                break;
            case carrier_unknown:
                break;
        }
        ppi_common->chan_flags = kis_htole16(ppi_common->chan_flags);

        ppi_common->fhss_hopset = 0;
        ppi_common->fhss_pattern = 0;

        ppi_common->signal_dbm = radioinfo->signal_dbm;
        ppi_common->noise_dbm = radioinfo->noise_dbm;
    }

    // Collate the allocation sizes of any callbacks
    for (unsigned int p = 0; p < ppilog->ppi_cb_vec.size(); p++) {
        // Ignore errors for now
        ppi_pos = (*(ppilog->ppi_cb_vec[p].cb))(0, in_pack, dump_data, ppi_pos,
                ppilog->ppi_cb_vec[p].aux);
    }

    dump_offset = ppi_pos;

    if (dump_len == 0) {
        delete[] dump_data;
        return 0;
    }

    if (dump_data == NULL)
        dump_data = new u_char[dump_len];

    // copy the packet content in, offset if necessary
    if (chunk != NULL) {
        memcpy(&(dump_data[dump_offset]), chunk->data, chunk->length);
        dump_offset += chunk->length;
    }

    // Lousy little hack to append the FCS after the data in PPI
    if (fcsdata != NULL && chunk != NULL && radioinfo != NULL) {
        memcpy(&(dump_data[dump_offset]), fcsdata->data, 4);
        dump_offset += 4;
    }

    // Fake a header
    struct pcap_pkthdr wh;
    wh.ts.tv_sec = in_pack->ts.tv_sec;
    wh.ts.tv_usec = in_pack->ts.tv_usec;
    wh.caplen = wh.len = dump_len;

    // Dump it
    {
        local_locker lock(&(ppilog->log_mutex));
        pcap_dump((u_char *) ppilog->dumper, &wh, dump_data);
    }

    delete[] dump_data;

    ppilog->log_packets++;
    ppilog->log_size += dump_len;

    return 1;
}

#endif /* have_libpcap */

