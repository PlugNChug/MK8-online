#ifndef MK8_BLOOM_MASK_H
#define MK8_BLOOM_MASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The 256 bloom_mask permutations in MK8 4.0.1 share 168 fragment programs.
// These are the renderer's existing program fingerprints, sorted for lookup.
static inline bool mk8_is_bloom_mask(uint64_t shader) {
    static const uint64_t programs[] = {
        0x00cb4d81523f76a2ull, 0x022397abec8fc474ull, 0x03860a3fe5be7c25ull, 0x041e61da60ef1061ull,
        0x051303030c68828full, 0x058bf573b612b4a5ull, 0x06b015df18988774ull, 0x06e81632d57117caull,
        0x085be66c3070ad73ull, 0x0b99055a652e5002ull, 0x0d499937ca3b31e3ull, 0x0e023d0b992b6ab1ull,
        0x0e1fa9b3719cf15bull, 0x0f09085281406346ull, 0x10658d52c75eec7aull, 0x1377a132cc21acb2ull,
        0x1403b4969cd53940ull, 0x159853c1fa8ffd54ull, 0x16c3aea2de90178bull, 0x16cbd1f17e7b74b5ull,
        0x1f9c227fcf42d7e5ull, 0x217274c4325f4242ull, 0x25901e542268d93bull, 0x26e677cef69e7968ull,
        0x285716743cab0290ull, 0x28df2132551ca82aull, 0x2b7208492ef48fe8ull, 0x2b9d418674930230ull,
        0x32b64dc9112a5456ull, 0x33d0340088ee6cb3ull, 0x37eb6f2fdfd33cd9ull, 0x3885113e04c5fa3full,
        0x3c5bc0d4af75f3d1ull, 0x3c8506eef35cce27ull, 0x3ce5c7132c799bbaull, 0x3d99cd350e34b9f9ull,
        0x3e198b131294c130ull, 0x43670e334223a70eull, 0x43fffb2ead84332full, 0x4556675671d52860ull,
        0x4942ebd278ca32b4ull, 0x4a6033f1d885b2c9ull, 0x4a8559bfd8f316b5ull, 0x4b22a527c5cb3212ull,
        0x4baeaeac7d1c1775ull, 0x4f3c2fcb8c9c0ca1ull, 0x502e86d8e7913576ull, 0x53ab926465221e5aull,
        0x58a34402d0d243d1ull, 0x59981005e027bfe2ull, 0x5aa125fc3c6af3adull, 0x5f984abe5a88f3f3ull,
        0x60a09acbc81e409full, 0x6271e8ba2f177492ull, 0x62f5778b0bafd597ull, 0x65320548b17c0d07ull,
        0x683635e84989ebcbull, 0x68846c7c75da2a4dull, 0x68dc33084e0af73full, 0x6a99d6caa64b06edull,
        0x6e53e787bfaeb7f8ull, 0x6e912c41b6664f24ull, 0x701b3389ab389ac4ull, 0x7216eeaec0362f0aull,
        0x726effd54376cba0ull, 0x7292d18ac84d26e9ull, 0x76781e70b39ffe24ull, 0x76befecda31f4718ull,
        0x76f2fbbb1fa95f9dull, 0x76f84f64b6c9151full, 0x775cd951f8aadc52ull, 0x7815b1a5a64d1621ull,
        0x78f669dca2a36551ull, 0x794ebf606f6801ddull, 0x7b2b30ed0dca8fe5ull, 0x7d4256be1f72da67ull,
        0x7e2ab15288aa8731ull, 0x7e654a14479ccd83ull, 0x7e7359f7dd1ce9d2ull, 0x7e818f5cb7500a0dull,
        0x81ec13cd43a53eb8ull, 0x835bcc98ab731470ull, 0x840cba3c3d609df1ull, 0x8650bbdb66c65085ull,
        0x8918401ebb3a1b95ull, 0x89548ee7896961caull, 0x89567744527d846cull, 0x89a8d5509079e928ull,
        0x8b33c8a84ec93fb3ull, 0x8bba46119a622467ull, 0x8ee8c0dba982d1d1ull, 0x8f09ec9e094df1b9ull,
        0x94097db3c23df046ull, 0x94897c0e6bfee48bull, 0x9636316fdc1cc487ull, 0x96be6ba525a56730ull,
        0x9a8a853faab65350ull, 0x9ac6002351d4af75ull, 0x9bb5715ab1e4b57bull, 0x9beda058b701b496ull,
        0xa02b0e182caff286ull, 0xa40f9c3b19842674ull, 0xa51e8fcda244d131ull, 0xa62118ca07034255ull,
        0xab238827c3683677ull, 0xad7e21c922552cecull, 0xae5395e0201d09efull, 0xafa273d67f65fc3dull,
        0xb12b5abd46d6c4e0ull, 0xb2082c2859b292e5ull, 0xb21ed1f995f7b07aull, 0xb337198cda7a194eull,
        0xb402f0150009bd80ull, 0xb4e2676921381a77ull, 0xb5318c5f9ef57fe5ull, 0xb5fe3a7e2ad77a99ull,
        0xb645b1ca048f9f20ull, 0xb6e5964bd86cbf85ull, 0xb7907b9bc8c1ccb0ull, 0xb93ac0e7d520b9d5ull,
        0xba4480f70f37c9e1ull, 0xbbfd127422901aa7ull, 0xbe4479bb1f8c69edull, 0xbee63bbfbc04a6edull,
        0xbfc431917426728bull, 0xc4ac9802841b7cfeull, 0xc4c80dcbb0de2e3full, 0xc516303c6425223eull,
        0xc6f23a14a71ffc98ull, 0xc7643b0dcc298c7cull, 0xc86bd82c2f518311ull, 0xc9c9fb1c10896e92ull,
        0xc9de5f9a95817d81ull, 0xcc332ddf5f06d544ull, 0xcd0bdd4047663747ull, 0xcd7e0e3a1ce13a7eull,
        0xcdec920424799cb7ull, 0xcf2169a9d25ea743ull, 0xcf97ab001bcdaec0ull, 0xd01e01948ed395d5ull,
        0xd23ccd465e0088adull, 0xd28b1d337beeba6eull, 0xd5ab49bee3496354ull, 0xd5df0613b2ad0a98ull,
        0xd71d0897b3ded100ull, 0xd8a3327fac870d8dull, 0xdc2ca8966fc7dd7aull, 0xde6ae2ce6ca6facaull,
        0xe0622e28c838333cull, 0xe11e8a623989643aull, 0xe15c1ef26200357aull, 0xe19417bc5d1fce89ull,
        0xe4705940e69d5466ull, 0xe5f60b4c5e2d891aull, 0xe62a0e83118cdf9eull, 0xe665e71ec91655e5ull,
        0xe70b7797044abd4aull, 0xea647c54266c7eb6ull, 0xec3924121390082aull, 0xec7288cc8213018cull,
        0xef9422c6406bf9ceull, 0xf1095ab12fecfb64ull, 0xf3d5389f855be5beull, 0xf6a3b1b932fa3053ull,
        0xfc44f2af7396c1d2ull, 0xfc9e1beae422c38aull, 0xfd2dd60feb02c640ull, 0xfeeae9ce74ab8af5ull,
    };
    size_t lo = 0, hi = sizeof programs / sizeof programs[0];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (shader == programs[mid]) return true;
        if (shader < programs[mid]) hi = mid;
        else lo = mid + 1;
    }
    return false;
}

// An opaque black RGB tint leaves the extraction shader's alpha and its draw intact.
// The later HDR composite still applies exposure, color grading and the HUD normally.
static inline const float *mk8_bloom_tint(bool enabled, uint64_t program, const float *original) {
    static const float black[8] = {0, 0, 0, 1, 0, 0, 0, 1};
    return !enabled && mk8_is_bloom_mask(program) ? black : original;
}

#endif
