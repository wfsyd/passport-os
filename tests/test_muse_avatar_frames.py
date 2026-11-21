#!/usr/bin/env python3
"""Pixel-exact check of the palette/RLE Muse avatar frames (both variants).

The SHA-256 digests are of the 30 original RGB565 frames (each frame as 4096
little-endian uint16 values, the layout LVGL reads). The C expander is built
host-side for each variant and its output is hashed frame by frame.
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "main" / "apps" / "muse"
FRAME_BYTES = 64 * 64 * 2
FRAME_COUNT = 30

# Original frame digests, computed from the pre-conversion arrays (git aea2c0d).
EXPECTED = {
    "muse_avatar_frames.c": (
        "60745097dd8add518e61fe4d92f6d712945b16e1f4d1fc24d19daba136e85a6d",
        "36a85d2ba4b843d43b2280a208165398a8e09da7ef0dfc06ebbe3b2c6989bff3",
        "27b3d654fe5c7caec7e75dba5fad7df2d351bacf1c5859834c9d217015c33ac1",
        "136bd5b058e9c2e996b56f83f25b4961c1947b0e80691e45515ed05b0aea1e5f",
        "cdf7457e7f4a4c59f801c0e36ab7040d664198c8700f6ebab2f8da27aa37a511",
        "c395d097b61bf3e021e93d8258424216eb1de2f65ff3f3277d99b96f3a26d093",
        "3ffbd79aa13ef3eab6ffd87eecac6ef04bd84010f20a1bbcf9a7344709a1f6a9",
        "6414582d27de2b1213aecad7e2ec364c23aa46a5f6942f7c56a9405422213da1",
        "9eb7e5704b14787d34d62326c6f0c0b502ed181a36faa09cd6c2c91b08569133",
        "e6f24af33ab4763a1d0905c8c4ff13f042b581eecd8cd26ea574271d46703f17",
        "770f55ed130d881fcaf99e0bef337b6737809ef41c5ea743a8dacad1f8390a4c",
        "cd54a671c610c65702d308fd9b46c3edc482730ec9249ef6b8d6eb46f6bfe977",
        "4f5cdf60b8ccd9aca8e15ca1ad326796c2a7289120baa746a17ce31be67a8570",
        "fba1e466a717118a9140484ba4c478ca45abcb478ba46189dae62e83d27a67a3",
        "5a60858b271e432dbb927278bec34921884becb21054166093d4786a9ac612f5",
        "87a655c7e6aaca6f923865301cec52562cd02a3a1270a1730df7860b0b997ce0",
        "4eaaa1736859fe84f6086c3a701709a96c6d4f9e28484053e21f9c09afd99421",
        "bc354cf8beafa96232da52b34998315ff3c19851eb5df9842ad9d030acd755cc",
        "d10d7dd8fb1d3f25f3f6c0d377d4882ffd284c3de203e75d8387e0d99f771922",
        "f92d6d521b752bd7de695790460e9246effa41bf92f0cce23b383427e9d3804b",
        "2a4f54f3532e867dc73fcf851294400604d0f51f7ddccb0d84426f76bf927532",
        "ce0b34579bf269debdb7374a0e05bee61074d91fa8853c196d2a914189e01f21",
        "a6b16bc1737c385e22c83cb36c013ccbc11f5702821957de8646fa329841f3ff",
        "5588cbc4cfdeeb677d60ac1cdd8ecaaa7d6afedffc1d3b2de35bd81ab2d7d718",
        "21f3b72ec7379d98b03ba7d07e96117941a1640fd277c32dac6cea07a2cc2b4a",
        "06159929bda499304edae1e8e58e8378cb6d01ebe03bfca744a0e456bb19025a",
        "8a0d4e8f67b498cd6628cc1cce4ae372ff59c68722ee84c0464034439ec0b0e8",
        "7519188adfa0e9fbfcb618d8a2f499359fc70ce6bafca4d8bf86b2845c90384c",
        "6cb17b6b8c1d204b34467efc7400c1df3dcadaab3aa4162f78d01961918eab5b",
        "8a97408383e9c5450f14dcda6ba5a91b2648d1d222c4c55d62a97b60d5ae9956",
    ),
    "muse_avatar_public_frames.c": (
        "b2625c584f79051f30a15f21188fbd05879e007c70db7b9171ba6df66585f787",
        "6a35c7dd1892dd1f5492f22d6b3b4c10710976259ab1430b8cf43452287ff72e",
        "6a35c7dd1892dd1f5492f22d6b3b4c10710976259ab1430b8cf43452287ff72e",
        "b2625c584f79051f30a15f21188fbd05879e007c70db7b9171ba6df66585f787",
        "cd170fd1d55ca261273645f487d45fdf6df7a0056ad4de96236bd73eaa989aa6",
        "e23196decabd296bcfd7e83b6e915ca316d4387810084a0b9b38ff3bedcea563",
        "ea91b3508ed69313f8133c184eb9372025d8c7b041250a962dbef2c281f49c1f",
        "a1d70abf10e809804c18d4fd7d923ec946785898a0a59a02d594fbb24860796d",
        "33131327bfc95294090ad62053205bcb272ed1edf6b4e408f9b14e22324ba8fd",
        "ed068314ed5b0ff5c309bf031bac8d9d4b30e60002c555e717de9c5f52e9c47c",
        "eba07872d9f8ff4a0a097ecfa36e7edd360d9cbbd910196655da18cd9b30c841",
        "0c90ef03c6e996a3258f7be19e143e5296c19b6c84165866e08b8e537eec7e5e",
        "40d57de07c98a44590faf9d7cdb3970ce2d619e2dd4bcaf6b964f85bebfbcacc",
        "0ca17ef1867e66eedbe78a52f4abf0252681ae85eaad13e7c7dc3e9a17ae15af",
        "9e8c8b1bd668cb18e3e5e4bffc7ab0c2a35a3b3a7dfa333332ae72d6c6ef5ca4",
        "9e8c8b1bd668cb18e3e5e4bffc7ab0c2a35a3b3a7dfa333332ae72d6c6ef5ca4",
        "722d909ac1c88c5db0595aa4d56e8598133474462193bf6671222d5739b22dfe",
        "25ec28b21afc828456be3c1f8808c5d4c54cf018c52c2c71e057c388733f0419",
        "09b5c17873117213706d9b5855e5058e9a94515a4bff10fa318a250606675f9b",
        "f3ab3d3cb3af8fbc824822c78247c3ba84cecad3cac61fb9d822c5c4c80bbe8a",
        "9e3d2d835aab00ab14ee2154148a1910196f148c0e1fb0f915c0c177769b86b2",
        "43a3ccc9e4b7e57f470c9cd206082cba79a93fe407673451da7dbc0405e235c2",
        "ea91b3508ed69313f8133c184eb9372025d8c7b041250a962dbef2c281f49c1f",
        "a1d70abf10e809804c18d4fd7d923ec946785898a0a59a02d594fbb24860796d",
        "1364f1a9725575005bc4221d21e71505096e6960ecd9f813757dd6d93548a5ee",
        "1364f1a9725575005bc4221d21e71505096e6960ecd9f813757dd6d93548a5ee",
        "f561a570ebc4be91216b82ae240155f77301c81108d62234bc706fbb62b2550c",
        "c990462962bc464708251ec6ae898468feb6b5829b7bd2cb22d381e1a03a8352",
        "2c6a7cd785a53e98a56e9a223e0272bb21cc3cf8e4765f221d8f11f2d41644f5",
        "0b301a532d983df10dfaff939b7c493a8354148fd722fad60cef6139ad74fde8",
    ),
}

DUMP_C = r'''
#include <stdio.h>
#include "muse_avatar_frames.h"
int main(void) {
    static uint16_t px[MUSE_AVATAR_FRAME_PIXELS];
    for (unsigned f = 0; f < MUSE_AVATAR_FRAME_COUNT; ++f) {
        if (!muse_avatar_expand_frame(f, px)) return 2;
        for (size_t i = 0; i < MUSE_AVATAR_FRAME_PIXELS; ++i) {
            unsigned char b[2] = {(unsigned char)(px[i] & 0xFF), (unsigned char)(px[i] >> 8)};
            if (fwrite(b, 1, 2, stdout) != 2) return 3;
        }
    }
    if (muse_avatar_expand_frame(MUSE_AVATAR_FRAME_COUNT, px)) return 4;
    if (muse_avatar_expand_frame(0, NULL)) return 5;
    return 0;
}
'''


def check_variant(source: str, expected: tuple, tmp: Path) -> None:
    dump_src = tmp / "dump.c"
    dump_src.write_text(DUMP_C, encoding="utf-8")
    exe = tmp / source.replace(".c", "")
    cc = os.environ.get("CC", "cc")
    subprocess.run(
        [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
         f"-I{MUSE}", str(dump_src), str(MUSE / source), "-o", str(exe)],
        check=True)
    result = subprocess.run([str(exe)], check=False, capture_output=True)
    if result.returncode != 0:
        raise SystemExit(f"{source}: harness exited {result.returncode}")
    data = result.stdout
    if len(data) != FRAME_COUNT * FRAME_BYTES:
        raise SystemExit(f"{source}: expected {FRAME_COUNT * FRAME_BYTES} bytes, got {len(data)}")
    if len(expected) != FRAME_COUNT:
        raise SystemExit(f"{source}: fixture has {len(expected)} digests")
    for index in range(FRAME_COUNT):
        frame = data[index * FRAME_BYTES:(index + 1) * FRAME_BYTES]
        digest = hashlib.sha256(frame).hexdigest()
        if digest != expected[index]:
            raise SystemExit(f"{source}: frame {index} differs from the original")


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="muse-avatar-") as tmp:
        for source, expected in EXPECTED.items():
            check_variant(source, expected, Path(tmp))
    print("Muse avatar palette frames: PASS")


if __name__ == "__main__":
    sys.exit(main())
