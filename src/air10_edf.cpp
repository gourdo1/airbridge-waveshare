#include "air10_edf.h"

#include <stdio.h>
#include <string.h>

#include "crc.h"

namespace Air10Edf {
namespace {

constexpr size_t EDF_FIXED_HEADER_SIZE = 256;
constexpr size_t EDF_SIGNAL_HEADER_SIZE = 256;
constexpr size_t EDF_PATIENT_OFFSET = 8;
constexpr size_t EDF_PATIENT_WIDTH = 80;
constexpr size_t EDF_PATIENT_CRC_SOURCE_OFFSET = 0x19;
constexpr size_t EDF_PATIENT_CRC_SOURCE_LENGTH = 0xe7;
constexpr size_t EDF_RECORD_COUNT_OFFSET = 236;
constexpr size_t EDF_RECORD_COUNT_WIDTH = 8;

const SignalSpec BRP_SIGNALS[] = {
    {"Flow.40ms", "L/s", "-2.00", "3.00", "-1000", "1500", 1500},
    {"Press.40ms", "cmH2O", "0.00", "40.00", "0", "2000", 1500},
    {"Crc16", "", "-32768", "32767", "-32768", "32767", 1},
};

const SignalSpec BRP_TCV_SIGNALS[] = {
    {"Flow.40ms", "L/s", "-2.00", "3.00", "-1000", "1500", 1500},
    {"Press.40ms", "cmH2O", "0.00", "40.00", "0", "2000", 1500},
    {"TrigCycEvt.40ms", "", "0", "16", "0", "16", 1500},
    {"Crc16", "", "-32768", "32767", "-32768", "32767", 1},
};

// RCR/RTR are deliberately omitted. Breath timing is selected per BRH schema.
const SignalSpec PLD_SIGNALS[] = {
    {"MaskPress.2s", "cmH2O", "0.00", "40.00", "0", "2000", 30},
    {"Press.2s", "cmH2O", "0.00", "30.00", "0", "1500", 30},
    {"EprPress.2s", "cmH2O", "0.00", "30.00", "0", "1500", 30},
    {"Leak.2s", "L/s", "0.00", "2.00", "0", "100", 30},
    {"RespRate.2s", "bpm", "0.00", "50.00", "0", "250", 30},
    {"TidVol.2s", "L", "0.00", "4.00", "0", "200", 30},
    {"MinVent.2s", "L/min", "0.00", "30.00", "0", "240", 30},
    {"TgtVent.2s", "L/min", "0.00", "30.00", "0", "240", 30},
    {"IERatio.2s", "%", "0.00", "200.00", "0", "200", 30},
    {"Snore.2s", "", "0.00", "5.00", "0", "250", 30},
    {"FlowLim.2s", "", "0.00", "1.00", "0", "100", 30},
    {"B5ITime.2s", "seconds", "0.00", "10.00", "0", "500", 30},
    {"B5ETime.2s", "seconds", "0.00", "10.00", "0", "500", 30},
    {"Ti.2s", "seconds", "0.00", "10.00", "0", "500", 30},
    {"AlvMinVent.2s", "L/min", "0.00", "30.00", "0", "240", 30},
    {"Crc16", "", "-32768", "32767", "-32768", "32767", 1},
};

const SignalSpec SAD_SIGNALS[] = {
    {"Pulse.1s", "bpm", "18.00", "300.00", "18", "300", 60},
    {"SpO2.1s", "%", "0.00", "100.00", "0", "100", 60},
    {"Crc16", "", "-32768", "32767", "-32768", "32767", 1},
};

const SignalSpec EVE_SIGNALS[] = {
    {"EDF Annotations", "", "0.00", "86400.00", "-32768", "32767", 19},
    {"Crc16", "", "-32768", "32767", "-32768", "32767", 1},
};

const SignalSpec CSL_SIGNALS[] = {
    {"EDF Annotations", "", "0.00", "86400.00", "-32768", "32767", 15},
    {"Crc16", "", "-32768", "32767", "-32768", "32767", 1},
};

// Airbreak SX567 STR superset. These bounds follow the EDF metadata rather
// than narrowing settings to the active model's current configuration range.
const SignalSpec STR_SIGNALS[] = {
    {"Date", "", "0.00", "24836.00", "0", "24836", 1},
    {"MaskOn", "min.", "0.00", "1440.00", "0", "1440", 10},
    {"MaskOff", "min.", "0.00", "1440.00", "0", "1440", 10},
    {"MaskEvents", "", "0.00", "255.00", "0", "255", 1},
    {"Duration", "min.", "0.00", "1440.00", "0", "1440", 1},
    {"OnDuration", "min.", "0.00", "1440.00", "0", "1440", 1},
    {"PatientHours", "hrs", "0.00", "65534.00", "0", "65534", 1},
    {"Mode", "", "0", "16", "0", "16", 1},
    {"S.RampEnable", "", "0", "16", "0", "16", 1},
    {"S.RampTime", "min.", "5.00", "45.00", "5", "45", 1},
    {"S.C.StartPress", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.C.Press", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.EPR.ClinEnable", "", "0", "16", "0", "16", 1},
    {"S.EPR.EPREnable", "", "0", "16", "0", "16", 1},
    {"S.EPR.Level", "cmH2O", "1.00", "3.00", "50", "150", 1},
    {"S.EPR.EPRType", "", "0", "16", "0", "16", 1},
    {"S.BL.StartPress", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.BL.IPAP", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.BL.EPAP", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.EasyBreathe", "", "0", "16", "0", "16", 1},
    {"S.VA.StartPress", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.VA.MaxIPAP", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.VA.MinEPAP", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.VA.PS", "cmH2O", "0.00", "30.00", "0", "1500", 1},
    {"S.RiseEnable", "", "0", "16", "0", "16", 1},
    {"S.RiseTime", "ms", "150.00", "900.00", "150", "900", 1},
    {"S.Cycle", "", "0", "16", "0", "16", 1},
    {"S.Trigger", "", "0", "16", "0", "16", 1},
    {"S.TiMax", "seconds", "0.30", "4.00", "15", "200", 1},
    {"S.TiMin", "seconds", "0.10", "4.00", "5", "200", 1},
    {"S.AS.Comfort", "", "0", "16", "0", "16", 1},
    {"S.AS.StartPress", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.AS.MaxPress", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.AS.MinPress", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.AV.StartPress", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.AV.EPAP", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.AV.MaxPS", "cmH2O", "0.00", "30.00", "0", "1500", 1},
    {"S.AV.MinPS", "cmH2O", "0.00", "30.00", "0", "1500", 1},
    {"S.AA.StartPress", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.AA.MaxEPAP", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.AA.MinEPAP", "cmH2O", "1.00", "30.00", "50", "1500", 1},
    {"S.AA.MaxPS", "cmH2O", "0.00", "30.00", "0", "1500", 1},
    {"S.AA.MinPS", "cmH2O", "0.00", "30.00", "0", "1500", 1},
    {"S.SmartStart", "", "0", "16", "0", "16", 1},
    {"S.PtAccess", "", "0", "16", "0", "16", 1},
    {"S.ABFilter", "", "0", "16", "0", "16", 1},
    {"S.LeakAlert", "", "0", "16", "0", "16", 1},
    {"S.Mask", "", "0", "16", "0", "16", 1},
    {"S.Tube", "", "0", "16", "0", "16", 1},
    {"S.ClimateControl", "", "0", "16", "0", "16", 1},
    {"S.HumEnable", "", "0", "16", "0", "16", 1},
    {"S.HumLevel", "", "1.00", "8.00", "1", "8", 1},
    {"S.TempEnable", "", "0", "16", "0", "16", 1},
    {"S.Temp", "\xC2\xB0" "C", "16.00", "30.00", "160", "300", 1},
    {"S.ExternalHum", "", "0", "16", "0", "16", 1},
    {"HeatedTube", "", "0", "16", "0", "16", 1},
    {"Humidifier", "", "0", "16", "0", "16", 1},
    {"BlowPress.95", "cmH2O", "0.00", "45.00", "0", "2250", 1},
    {"BlowPress.5", "cmH2O", "0.00", "45.00", "0", "2250", 1},
    {"Flow.95", "L/s", "0.00", "3.00", "0", "1500", 1},
    {"Flow.5", "L/s", "0.00", "3.00", "0", "1500", 1},
    {"BlowFlow.50", "L/min", "0.00", "250.00", "0", "25000", 1},
    {"AmbHumidity.50", "mg/L", "0.00", "100.00", "0", "100", 1},
    {"HumTemp.50", "\xC2\xB0" "C", "0.00", "95.00", "0", "950", 1},
    {"HTubeTemp.50", "\xC2\xB0" "C", "0.00", "40.00", "0", "400", 1},
    {"HTubePow.50", "", "0.00", "100.00", "0", "100", 1},
    {"HumPow.50", "", "0.00", "100.00", "0", "100", 1},
    {"SpO2.50", "%", "0.00", "100.00", "0", "100", 1},
    {"SpO2.95", "%", "0.00", "100.00", "0", "100", 1},
    {"SpO2.Max", "%", "0.00", "100.00", "0", "100", 1},
    {"SpO2Thresh", "min.", "0.00", "1440.00", "0", "1440", 1},
    {"CSR", "", "0.00", "1440.00", "0", "1440", 1},
    {"SpontCyc%", "%", "0.00", "100.00", "0", "200", 1},
    {"MaskPress.50", "cmH2O", "0.00", "30.00", "0", "1500", 1},
    {"MaskPress.95", "cmH2O", "0.00", "30.00", "0", "1500", 1},
    {"MaskPress.Max", "cmH2O", "0.00", "30.00", "0", "1500", 1},
    {"TgtIPAP.50", "cmH2O", "0.00", "30.00", "0", "1500", 1},
    {"TgtIPAP.95", "cmH2O", "0.00", "30.00", "0", "1500", 1},
    {"TgtIPAP.Max", "cmH2O", "0.00", "30.00", "0", "1500", 1},
    {"TgtEPAP.50", "cmH2O", "0.00", "30.00", "0", "1500", 1},
    {"TgtEPAP.95", "cmH2O", "0.00", "30.00", "0", "1500", 1},
    {"TgtEPAP.Max", "cmH2O", "0.00", "30.00", "0", "1500", 1},
    {"Leak.50", "L/s", "0.00", "2.00", "0", "100", 1},
    {"Leak.95", "L/s", "0.00", "2.00", "0", "100", 1},
    {"Leak.70", "L/s", "0.00", "2.00", "0", "100", 1},
    {"Leak.Max", "L/s", "0.00", "2.00", "0", "100", 1},
    {"MinVent.50", "L/min", "0.00", "30.00", "0", "240", 1},
    {"MinVent.95", "L/min", "0.00", "30.00", "0", "240", 1},
    {"MinVent.Max", "L/min", "0.00", "30.00", "0", "240", 1},
    {"RespRate.50", "bpm", "0.00", "50.00", "0", "250", 1},
    {"RespRate.95", "bpm", "0.00", "50.00", "0", "250", 1},
    {"RespRate.Max", "bpm", "0.00", "50.00", "0", "250", 1},
    {"TidVol.50", "L", "0.00", "4.00", "0", "200", 1},
    {"TidVol.95", "L", "0.00", "4.00", "0", "200", 1},
    {"TidVol.Max", "L", "0.00", "4.00", "0", "200", 1},
    {"IERatio.50", "%", "0.00", "200.00", "0", "200", 1},
    {"IERatio.95", "%", "0.00", "200.00", "0", "200", 1},
    {"IERatio.Max", "%", "0.00", "200.00", "0", "200", 1},
    {"Ti.50", "seconds", "0.00", "10.00", "0", "500", 1},
    {"Ti.95", "seconds", "0.00", "10.00", "0", "500", 1},
    {"Ti.Max", "seconds", "0.00", "10.00", "0", "500", 1},
    {"TgtVent.50", "L/min", "0.00", "30.00", "0", "240", 1},
    {"TgtVent.95", "L/min", "0.00", "30.00", "0", "240", 1},
    {"TgtVent.Max", "L/min", "0.00", "30.00", "0", "240", 1},
    {"AHI", "", "0.00", "240.00", "0", "2400", 1},
    {"HI", "", "0.00", "240.00", "0", "2400", 1},
    {"AI", "", "0.00", "240.00", "0", "2400", 1},
    {"OAI", "", "0.00", "240.00", "0", "2400", 1},
    {"CAI", "", "0.00", "240.00", "0", "2400", 1},
    {"UAI", "", "0.00", "240.00", "0", "2400", 1},
    {"RIN", "", "0.00", "240.00", "0", "2400", 1},
    {"Fault.Device", "", "0", "16", "0", "16", 1},
    {"Fault.Alarm", "", "0", "16", "0", "16", 1},
    {"Fault.Humidifier", "", "0", "16", "0", "16", 1},
    {"Fault.HeatedTube", "", "0", "16", "0", "16", 1},
    {"S.BL.BackupRate", "bpm", "0.00", "50.00", "0", "250", 1},
    {"S.BL.RespRate", "bpm", "5.00", "50.00", "25", "250", 1},
    {"S.Ti", "seconds", "0.30", "4.00", "15", "200", 1},
    {"SpontTrig%", "%", "0.00", "100.00", "0", "200", 1},
    {"Crc16", "", "-32768", "32767", "-32768", "32767", 1},
};

const char *const STR_TAGS[] = {
    "LSD", "ONT", "OFT", "MSE", "OND", "THD", "PHM", "MOP",
    "RMA", "RMT", "STP", "IPC", "EPA", "EPX", "EPR", "EPT",
    "EPS", "IPP", "EPP", "EBE", "STV", "MXI", "MNE", "SPT",
    "RSC", "RST", "VCS", "VTS", "ITX", "ITN",
    "AFC", "STU", "MPA", "MPI",
    "STE", "EEP", "MXS", "MNS", "EAS", "EAX", "EAI", "AXS", "ANS",
    "SST", "ACC", "ABF", "ALR", "MSK", "TBT", "CCO", "HMX",
    "HMS", "HTX", "HTS", "HME", "HTB", "HUM",
    "BP9", "BP5", "RF9", "RF5", "BFM", "ABM", "HHM", "HTM",
    "TPM", "HPM", "SOM", "SO9", "SOX", "SAU", "CSD", "VCR",
    "MSP", "PM9",
    "PMA", "PIM", "PI9", "PIA", "PEM", "PE9", "PEA", "LKM",
    "LK9", "LK7", "LMX", "VTM", "VT9", "VTA", "RRM", "RR9",
    "RRA", "TVM", "TV9", "TVA", "IEM", "IE9", "IEA", "ISM",
    "IS9", "ISA", "VAM", "VA9", "VAA", "AHI", "HIS", "AIS",
    "CLI", "OPI", "UAI", "RIN", "SYS", "SYT", "SYC", "SYH",
    "BRR", "RRT", "ITT", "VSR", "DCR",
};

const Schema BRP = {FileKind::BRP, "BRP", "EDF", BRP_SIGNALS, 3, 0, 60};
const Schema BRP_TCV = {FileKind::BRP, "BRP", "EDF", BRP_TCV_SIGNALS, 4, 0, 60};
const Schema PLD = {
    FileKind::PLD, "PLD", "EDF", PLD_SIGNALS,
    sizeof(PLD_SIGNALS) / sizeof(PLD_SIGNALS[0]), 0, 60,
};
const Schema SAD = {FileKind::SAD, "SAD", "EDF", SAD_SIGNALS, 3, 0, 60};
const Schema EVE = {FileKind::EVE, "EVE", "EDF+D", EVE_SIGNALS, 2, 38, 0};
const Schema CSL = {FileKind::CSL, "CSL", "EDF+D", CSL_SIGNALS, 2, 30, 0};
const Schema STR = {
    FileKind::STR, "STR", "EDF", STR_SIGNALS, 120, 0, 86400,
};

static_assert(sizeof(STR_SIGNALS) / sizeof(STR_SIGNALS[0]) ==
                  sizeof(STR_TAGS) / sizeof(STR_TAGS[0]),
              "STR tags and signals must stay aligned");

void append_field(uint8_t *dst, size_t capacity, size_t &offset,
                  const char *text, size_t width) {
    if (!dst || offset + width > capacity) return;
    memset(dst + offset, ' ', width);
    if (text) memcpy(dst + offset, text, strnlen(text, width));
    offset += width;
}

void append_u32(uint8_t *dst, size_t capacity, size_t &offset,
                uint32_t value, size_t width) {
    char text[16];
    snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(value));
    append_field(dst, capacity, offset, text, width);
}

bool append_bytes(uint8_t *dst, size_t capacity, size_t &offset,
                  const void *src, size_t len) {
    if (!dst || !src || offset + len > capacity) return false;
    memcpy(dst + offset, src, len);
    offset += len;
    return true;
}

bool append_byte(uint8_t *dst, size_t capacity, size_t &offset, uint8_t value) {
    return append_bytes(dst, capacity, offset, &value, 1);
}

bool append_i16_le(uint8_t *dst, size_t capacity, size_t &offset, int16_t value) {
    uint8_t bytes[2] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8),
    };
    return append_bytes(dst, capacity, offset, bytes, sizeof(bytes));
}

void finalize_patient_id(uint8_t *header, size_t size) {
    memset(header + EDF_PATIENT_OFFSET, ' ', EDF_PATIENT_WIDTH);
    memcpy(header + EDF_PATIENT_OFFSET, "X X X X 0000 0000", 17);

    const uint16_t fixed_crc = crc16_ccitt(
        header + EDF_PATIENT_CRC_SOURCE_OFFSET,
        EDF_PATIENT_CRC_SOURCE_LENGTH);
    const uint16_t signal_crc = crc16_ccitt(
        header + EDF_FIXED_HEADER_SIZE, size - EDF_FIXED_HEADER_SIZE);

    char patient[24];
    snprintf(patient, sizeof(patient), "X X X X %04X %04X",
             static_cast<unsigned>(fixed_crc),
             static_cast<unsigned>(signal_crc));
    memcpy(header + EDF_PATIENT_OFFSET, patient, strlen(patient));
}

}  // namespace

const Schema &brp_schema(bool include_tcv) {
    return include_tcv ? BRP_TCV : BRP;
}

const Schema &pld_schema() { return PLD; }

Schema pld_schema(bool include_int, bool include_ext,
                  SignalSpec (&signals)[PLD_MAX_SIGNALS]) {
    static_assert(sizeof(PLD_SIGNALS) / sizeof(PLD_SIGNALS[0]) == PLD_MAX_SIGNALS);
    Schema selected = PLD;
    selected.signals = signals;
    selected.signal_count = 0;
    for (const SignalSpec &signal : PLD_SIGNALS) {
        if (!include_int && (!strcmp(signal.label, "Ti.2s") ||
                             !strcmp(signal.label, "B5ITime.2s"))) continue;
        if (!include_ext && !strcmp(signal.label, "B5ETime.2s")) continue;
        signals[selected.signal_count++] = signal;
    }
    return selected;
}

const Schema &sad_schema() { return SAD; }
const Schema &eve_schema() { return EVE; }
const Schema &csl_schema() { return CSL; }
const Schema &str_schema() { return STR; }

const char *str_signal_tag(size_t signal_index) {
    return signal_index < sizeof(STR_TAGS) / sizeof(STR_TAGS[0])
               ? STR_TAGS[signal_index] : nullptr;
}

size_t header_size(const Schema &schema) {
    return EDF_FIXED_HEADER_SIZE +
           static_cast<size_t>(schema.signal_count) * EDF_SIGNAL_HEADER_SIZE;
}

size_t record_size(const Schema &schema) {
    size_t samples = 0;
    for (uint8_t i = 0; i < schema.signal_count; i++)
        samples += schema.signals[i].samples_per_record;
    return samples * sizeof(int16_t);
}

size_t numeric_sample_count(const Schema &schema) {
    if (schema.annotation_payload_bytes || schema.signal_count < 2) return 0;
    size_t samples = 0;
    for (uint8_t i = 0; i + 1 < schema.signal_count; i++)
        samples += schema.signals[i].samples_per_record;
    return samples;
}

bool render_header(const Schema &schema, const HeaderInfo &info,
                   uint8_t *dst, size_t capacity, size_t &written) {
    written = 0;
    const size_t required = header_size(schema);
    if (!dst || !schema.signals || !schema.signal_count || capacity < required)
        return false;

    memset(dst, ' ', required);
    size_t offset = 0;
    append_field(dst, capacity, offset, "0", 8);
    append_field(dst, capacity, offset, "", 80);
    append_field(dst, capacity, offset, info.recording_id, 80);
    append_field(dst, capacity, offset, info.start_date, 8);
    append_field(dst, capacity, offset, info.start_time, 8);
    append_u32(dst, capacity, offset, static_cast<uint32_t>(required), 8);
    append_field(dst, capacity, offset, schema.reserved, 44);
    append_u32(dst, capacity, offset, info.record_count, 8);
    append_field(dst, capacity, offset,
                 schema.record_duration_seconds == 86400 ? "86400.00" :
                 schema.record_duration_seconds ? "60.00" : "0.00", 8);
    append_u32(dst, capacity, offset, schema.signal_count, 4);

    for (uint8_t i = 0; i < schema.signal_count; i++)
        append_field(dst, capacity, offset, schema.signals[i].label, 16);
    for (uint8_t i = 0; i < schema.signal_count; i++)
        append_field(dst, capacity, offset, "", 80);
    for (uint8_t i = 0; i < schema.signal_count; i++)
        append_field(dst, capacity, offset, schema.signals[i].dimension, 8);
    for (uint8_t i = 0; i < schema.signal_count; i++)
        append_field(dst, capacity, offset, schema.signals[i].physical_min, 8);
    for (uint8_t i = 0; i < schema.signal_count; i++)
        append_field(dst, capacity, offset, schema.signals[i].physical_max, 8);
    for (uint8_t i = 0; i < schema.signal_count; i++)
        append_field(dst, capacity, offset, schema.signals[i].digital_min, 8);
    for (uint8_t i = 0; i < schema.signal_count; i++)
        append_field(dst, capacity, offset, schema.signals[i].digital_max, 8);
    for (uint8_t i = 0; i < schema.signal_count; i++)
        append_field(dst, capacity, offset, "", 80);
    for (uint8_t i = 0; i < schema.signal_count; i++)
        append_u32(dst, capacity, offset, schema.signals[i].samples_per_record, 8);
    for (uint8_t i = 0; i < schema.signal_count; i++)
        append_field(dst, capacity, offset, "", 32);

    if (offset != required) return false;
    finalize_patient_id(dst, required);
    written = required;
    return true;
}

bool update_record_count(uint8_t *header, size_t size,
                         uint32_t record_count) {
    if (!header || size < EDF_FIXED_HEADER_SIZE ||
        (size - EDF_FIXED_HEADER_SIZE) % EDF_SIGNAL_HEADER_SIZE != 0) {
        return false;
    }

    char text[16];
    const int len = snprintf(text, sizeof(text), "%lu",
                             static_cast<unsigned long>(record_count));
    if (len <= 0 || static_cast<size_t>(len) > EDF_RECORD_COUNT_WIDTH)
        return false;
    memset(header + EDF_RECORD_COUNT_OFFSET, ' ', EDF_RECORD_COUNT_WIDTH);
    memcpy(header + EDF_RECORD_COUNT_OFFSET, text, len);
    finalize_patient_id(header, size);
    return true;
}

bool render_numeric_record(const Schema &schema, const int16_t *samples,
                           size_t sample_count, uint8_t *dst,
                           size_t capacity, size_t &written) {
    written = 0;
    const size_t required = record_size(schema);
    if (!samples || sample_count != numeric_sample_count(schema) ||
        !dst || capacity < required) {
        return false;
    }

    size_t offset = 0;
    for (size_t i = 0; i < sample_count; i++)
        if (!append_i16_le(dst, capacity, offset, samples[i])) return false;
    const uint16_t crc = crc16_ccitt(dst, offset);
    if (!append_i16_le(dst, capacity, offset, static_cast<int16_t>(crc)))
        return false;
    written = offset;
    return offset == required;
}

bool render_annotation_record(const Schema &schema, uint32_t onset_seconds,
                              uint32_t duration_seconds, const char *label,
                              uint8_t *dst, size_t capacity, size_t &written) {
    written = 0;
    if (!schema.annotation_payload_bytes || !label ||
        capacity < record_size(schema)) {
        return false;
    }

    uint8_t payload[38] = {};
    if (schema.annotation_payload_bytes > sizeof(payload)) return false;
    size_t offset = 0;
    char text[16];
    if (!append_bytes(payload, sizeof(payload), offset, "+0", 2) ||
        !append_byte(payload, sizeof(payload), offset, 0x14) ||
        !append_byte(payload, sizeof(payload), offset, 0x14) ||
        !append_byte(payload, sizeof(payload), offset, 0x00)) {
        return false;
    }
    snprintf(text, sizeof(text), "+%lu", static_cast<unsigned long>(onset_seconds));
    if (!append_bytes(payload, schema.annotation_payload_bytes, offset,
                      text, strlen(text)) ||
        !append_byte(payload, schema.annotation_payload_bytes, offset, 0x15)) {
        return false;
    }
    snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(duration_seconds));
    if (!append_bytes(payload, schema.annotation_payload_bytes, offset,
                      text, strlen(text)) ||
        !append_byte(payload, schema.annotation_payload_bytes, offset, 0x14) ||
        !append_bytes(payload, schema.annotation_payload_bytes, offset,
                      label, strlen(label)) ||
        !append_byte(payload, schema.annotation_payload_bytes, offset, 0x14) ||
        !append_byte(payload, schema.annotation_payload_bytes, offset, 0x00)) {
        return false;
    }

    memcpy(dst, payload, schema.annotation_payload_bytes);
    size_t out = schema.annotation_payload_bytes;
    const uint16_t crc = crc16_ccitt(payload, schema.annotation_payload_bytes);
    if (!append_i16_le(dst, capacity, out, static_cast<int16_t>(crc)))
        return false;
    written = out;
    return out == record_size(schema);
}

}  // namespace Air10Edf
