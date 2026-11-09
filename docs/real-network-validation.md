> Historical record: this describes an earlier source revision, not current certification. See [current quality validation](engineering/quality-validation.md).

# Gerçek ağ doğrulaması — 2026-11-03

## Ortam ve kapsam

PASS — macOS 27.2 (26B123), arm64; Apple clang 21.0.0
(clang-2100.4.1.1), Xcode toolchain. Wi-Fi en0 aktif:
192.168.31.116/24, subnet 192.168.31.0/24, broadcast 192.168.31.255.
`route -n get default` gateway 192.168.31.1 ve interface en0 döndürdü.
lo0 loopback; awdl0 da aktif. Ethernet/Thunderbolt arayüzleri inactive;
utun arayüzleri mevcut. IPv4 varsayılan rota en0 üzerinden.

İlk sandbox denemesinde networksetup AuthorizationCreate -60008,
route socket EPERM, loopback bind EPERM verdi (BLOCKED).
Açık izin mekanizmasıyla sandbox dışında tekrar çalıştırılan sorgular ve
fixture başarılı oldu. Firewall veya cihaz yapılandırması değiştirilmedi.

Güncel derleme: macOS Make ve offline CMake hedefleri `NERO_NETWORK_UNAVAILABLE`
ile komut katmanında trafik üretmeden kullanılamıyor mesajı verir; CLI devam eder.
Linux kaynaklarındaki raw TCP/UDP trafik üretimi ve portscanner macOS üretim
ikilisinde çalıştırılmadı. Arayüz seçimi getifaddrs ile salt okunur yapılır;
seçimin socket binding anlamına gelmediğine dikkat edilmelidir.

## Gerçek sonuçlar

| Test | Durum | Ölçüm |
|---|---|---|
| macOS üretim derlemesi | PASS | make ve iki CMake preset build tamamlandı |
| Gerçek arayüz listesi/seçimi | PASS | en0 liste indeksi 15; SELECTED:en0, exit 0 |
| Geçersiz seçim | PASS | 0, 999, 1junk için exit 1 |
| Loopback TCP | PASS | 25 bağlantı, veri alışverişi, peer kapandıktan sonra EOF |
| Loopback UDP | PASS | 25 döngü çift yönlü veri alışverişi |
| UDP timeout | PASS | her döngü 30 ms receive timeout |
| TCP connect timeout | PASS | dinlemeyen yerel porta 1 saniyelik timeout |
| ECONNREFUSED | BLOCKED | ortam non-listening bound port için timeout verdi |
| Socket descriptor temizliği | PASS | TCP/UDP ve timeout sonrasında açık FD kümesi [0,1,2] |
| Yerel resolver | PASS | localhost getaddrinfo başarılı; hatalı numeric adres EAI_NONAME=8 |
| Resolver kaynakları | PASS | ilk çağrıda FD 3,4 eklendi; sonraki 25 çağrıda küme sabit |
| Gateway ICMP | PASS | 2/2 yanıt, kayıp %0, ortalama 3.025 ms |
| Gateway HTTP | PASS | yalnızca TCP/80 doğrulandı; tek HEAD /, HTTP 200, nginx |
| Üretim CLI | PASS | version, desteklenmeyen Syn komutu, exit: süreç kodu 0 |
| Üretim SIGINT/SIGTERM | PASS | bekleyen CLI sırasıyla 130/143 ile sonlandı |
| make test | PASS | safe regression ve 19 terminal testi başarılı |
| ctest --preset macos | PASS | 4/4 |
| ctest --preset macos-sanitize | PASS | 4/4, ASan/UBSan hata raporlamadı |
| Uzak erişilemeyen hedef | BLOCKED | yetki sınırı nedeniyle dış hedef veya diğer LAN cihazı denenmedi |
| Nero gerçek TCP/UDP motoru | BLOCKED | macOS uygulaması bu işlevleri desteklemiyor |
| Yoğun trafik/scan motorunun izolasyon testi | BLOCKED | no-network adapter/mock test edildi; gerçek Linux motoru yürütülmedi |

TCP/UDP sonuçları Python fixture ile işletim sistemi socket testleridir;
Nero ağ motorunun uçtan uca başarısı olarak yorumlanmamalıdır.
İlk fixture koşusu refusal beklediği için başarısız oldu. Beklenti gözlenen
ortam timeoutunu ayrı raporlayacak şekilde düzeltildi; refusal PASS yapılmadı.
İkinci koşuda resolver başlangıç descriptorları yanlışlıkla socket sızıntısı
sayıldı. Socket kontrolü resolver öncesine taşındı ve resolver için ayrı
25 çağrılık kararlılık kontrolü eklendi. Ürün kodunda doğrulanmış yeni hata yok;
mevcut kullanıcı değişiklikleri korunmuştur.

## Çalıştırılan komutlar ve kanıt

- `sw_vers`, `uname -m`, `clang --version`, `ifconfig`
- `networksetup -listallhardwareports`, `route -n get default`
- `make`, `cmake --preset macos`, `cmake --build --preset macos`
- `cmake --preset macos-sanitize`, `cmake --build --preset macos-sanitize`
- `python3 tests/real_network_validation.py` (sandbox engeli sonrası izinli tekrarlar)
- `ping -c 2 -W 1000 192.168.31.1`
- `nc -z -G 2 192.168.31.1 80`
- `curl --noproxy '*' --max-time 3 -I http://192.168.31.1/`
- `make test`, `ctest --preset macos`, `ctest --preset macos-sanitize`
- `git diff --check`

Ham çıktılar: [runtime](validation-logs/runtime.log),
[ilk fixture sonucu](validation-logs/runtime-first.log),
[gateway](validation-logs/gateway.log), [build](validation-logs/build.log),
[make test](validation-logs/make-test.log),
[CTest](validation-logs/ctest-macos.log),
[sanitizer](validation-logs/ctest-sanitize.log).

## Kalan riskler

ASan/UBSan yalnızca yürütülen C yollarında buffer/pointer/integer UB hatası
raporlamadı; tüm bellek hatalarının veya sızıntıların yokluğunu kanıtlamaz.
Fixture Python socket yaşam döngüsünü ölçer; Linux motorunu kapsamaz.
FD sayımı 0–4095 aralığındadır. Sinyal testi üretim CLI boşta beklerken yapıldı;
aktif Nero bağlantısı sırasında cleanup macOS desteği olmadığı için BLOCKED.
DNS testi yerel resolver ve numeric hata yoludur; dış DNS sorgusu/timeoutu
BLOCKED. Bağlantı reddinin timeout olmasının ağ filtresi kaynaklı olup olmadığı
belirlenmedi; ayarlar değiştirilmedi. Commit/push yapılmadı.
