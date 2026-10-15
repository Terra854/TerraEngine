"""Pure protocol checks only: no fabricated GPU receipt or acceptance evidence."""
import importlib.util
import json
from pathlib import Path
import struct
import zlib

path = Path(__file__).with_name("Compare-RuntimeMemorySafetyParity.py")
spec = importlib.util.spec_from_file_location("rms_parity", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

# Every channel participates, including alpha; equality and exact2 pass while
# one3-step alpha discrepancy fails. No screenshot or native output is invented.
zero = module.pixel_compare(bytes((10, 20, 30, 40)), bytes((10, 20, 30, 40)))
assert zero["comparedChannels"] == 4 and zero["maximumChannelError"] == 0
boundary = module.pixel_compare(bytes((10, 20, 30, 40)), bytes((12, 18, 32, 38)))
assert boundary["channelsExceedingTolerance"] == 0 and boundary["maximumNormalizedChannelError"] == 2 / 255
outside = module.pixel_compare(bytes((10, 20, 30, 40)), bytes((10, 20, 30, 43)))
assert outside["channelsExceedingTolerance"] == 1 and outside["firstFailure"]["channel"] == 3

for text in ('{"same":1,"same":2}', '{"bad":NaN}', '{"bad":Infinity}'):
	try:
		json.loads(text, object_pairs_hook=module.pairs, parse_constant=lambda _: (_ for _ in ()).throw(module.Unverified("nonfinite")))
	except module.Unverified:
		pass
	else:
		raise AssertionError("Ambiguous/nonfinite receipt accepted")
try:
	module.integer(True, "boolean counter")
except module.Unverified:
	pass
else:
	raise AssertionError("Boolean identity accepted as integer")

pixels = bytes((1, 2, 3, 4, 5, 6, 7, 8))
encoded = module.png(pixels, 2, 1)
assert encoded[:8] == b"\x89PNG\r\n\x1a\n"
offset = 8
compressed = b""
while offset < len(encoded):
	size = struct.unpack(">I", encoded[offset:offset + 4])[0]
	kind = encoded[offset + 4:offset + 8]
	body = encoded[offset + 8:offset + 8 + size]
	assert struct.unpack(">I", encoded[offset + 8 + size:offset + 12 + size])[0] == zlib.crc32(kind + body) & 0xffffffff
	if kind == b"IDAT":
		compressed += body
	offset += size + 12
assert zlib.decompress(compressed) == b"\0" + pixels
# Exercise the independent native PNG decoder, including alpha and strict CRC,
# extent and trailing-data rejection. This remains a CPU protocol fixture.
assert module.decode_native_png(encoded, 2, 1) == pixels
for invalid, width, height in ((encoded[:-1], 2, 1), (encoded + b"x", 2, 1), (encoded, 1, 1), (encoded[:29] + bytes((encoded[29] ^ 1,)) + encoded[30:], 2, 1)):
	try:
		module.decode_native_png(invalid, width, height)
	except module.Unverified:
		pass
	else:
		raise AssertionError("Corrupt final normal PNG accepted")
print("PASS: complete pixel/alpha boundary, strict JSON/integer rejection, PNG exact-byte roundtrip/native decoder rejection (supporting protocol checks only)")
