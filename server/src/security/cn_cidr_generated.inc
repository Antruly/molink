// Generated mainland-China IPv4 ranges.
//
// Source: APNIC delegated statistics (ftp.apnic.net/stats/apnic).
// Regenerate with: python3 -I server/tools/gen-cn-cidr.py
//
// This checked-in sample is a placeholder until the generator is run against a
// fresh snapshot; it keeps the classifier and its tests meaningful offline.
// The generator replaces this whole file and writes the snapshot date/hash at
// the top.
static constexpr cn_range kCnRanges[] = {
    {0x01000100u, 0x010001ffu},  // 1.0.1.0/24
    {0x01000200u, 0x010003ffu},  // 1.0.2.0/23
    {0x0e000000u, 0x0effffffu},  // 14.0.0.0/8
    {0x65000000u, 0x65ffffffu},  // 101.0.0.0/8
    {0x72727272u, 0x72727272u},  // 114.114.114.114/32 (public DNS)
    {0x77d7d700u, 0x77d7d7ffu},  // 119.29.29.0/24 (public DNS)
    {0xb44c0000u, 0xb44cffffu},  // 180.76.0.0/16
    {0xdf050500u, 0xdf0505ffu},  // 223.5.5.0/24 (public DNS)
};
static constexpr std::size_t kCnRangeCount =
    sizeof(kCnRanges) / sizeof(kCnRanges[0]);
