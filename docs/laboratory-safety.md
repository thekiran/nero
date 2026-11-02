# Laboratory safety

**EXPERIMENTAL SOFTWARE — LABORATORY USE ONLY**

Nero is an experimental network research and testing project intended exclusively
for controlled, authorized laboratory environments. It is not intended for public
use, production deployment or unauthorized network activity.

**DENEYSEL YAZILIM — YALNIZCA LABORATUVAR KULLANIMI**

Yalnızca izole veya açıkça test izni verilmiş ortamlarda eğitim, araştırma ve
uyumluluk doğrulaması yapılmalıdır. Genel kullanım veya üretim dağıtımı amaçlanmaz.

Compatibility tests must not execute the Linux engine or scanner, including against
loopback. CMake development targets never link either module. The unavailable
adapter ignores all target strings and returns failure. Interface tests read local
metadata; fixtures supply synthetic records. No DNS lookup, connect, ping, packet
transmission, privilege change or system-wide install belongs in a test.

The legacy Linux Make binary retains its original behavior and is **not** a safe
substitute for the separate `nero-safe` test binary. Build it for compatibility;
do not execute its traffic-producing commands in CI. No throughput or attack
capability changes are within migration scope.
