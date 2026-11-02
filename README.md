# Nero

C ile yazılmış deneysel terminal ve ağ araştırma projesi.
Yalnızca izole veya açıkça yetkilendirilmiş laboratuvar ortamları içindir.

## Nereden başlamalıyım?

| İhtiyaç | Başlangıç noktası |
| --- | --- |
| Kodda ne nerede? | [Kod haritası](docs/code-map.md) |
| Derlemek ve test etmek | [Geliştirme rehberi](docs/building.md) |
| Bileşenlerin ilişkisi | [Mimari](docs/architecture.md) |
| Yeni kod eklemek | [Katkı kuralları](CONTRIBUTING.md) |
| Belgelerin tamamı | [Doküman dizini](docs/README.md) |

## Hızlı başlangıç — macOS

Depo kökünde:

```sh
make             # .build/ altında derler, kökte nero oluşturur
./nero           # Terminal arayüzünü açar
make test        # Ağ işlemi yapmayan testleri çalıştırır
make help        # Geliştirme komutlarını listeler
```

C derleyicisi ve SDK gerekir; testler ayrıca Python 3 kullanır.
CMake ile geliştirme için [kurulum ve preset komutlarına](docs/building.md) bakın.

## Kaynak düzeni

```text
nero/
├── src/
│   ├── main.c                  Uygulamanın giriş noktası
│   ├── cli/                    Terminal oturumu, giriş, çizim ve komutlar
│   ├── core/                   Platform bilgisi ve ortak temel kod
│   ├── modules/                Platform raporu ve mevcut Linux modülleri
│   └── platform/               İşletim sistemi adaptörleri
├── include/nero/               Dışarıya açık proje başlıkları
├── third_party/cjson/          Üçüncü taraf kodu
├── tests/                     Regresyon, birim, entegrasyon ve mock testleri
├── cmake/                     Ortak CMake hedef ayarları
├── docs/                      Kod haritası ve geliştirme belgeleri
├── data/                      Veri dosyaları
└── .build/                    Üretilen çıktılar; Git dışında
```

`src/cli/cli_internal.h` yalnızca CLI dosyaları arasındaki özel sözleşmedir.
Dış bileşenler `include/nero/` başlıklarını kullanır.

## Platform kapsamı

| Platform | Make | CMake |
| --- | --- | --- |
| macOS | Terminal ve yerel arayüz adaptörü; ağ işlemleri devre dışı | Çevrimdışı CLI ve platform testleri |
| Linux | Mevcut Linux modülleri | Çevrimdışı CLI ve platform testleri |
| Windows | Desteklenmiyor | Platform raporu ve temel testler; CLI yok |

Yerelde macOS doğrulanmıştır. Linux ve Windows CI tanımları, bu platformlarda
başarılı çalıştırma kanıtı değildir. Ayrıntılar: [platform kapsamı](docs/platform-support.md).

[Lisans](LICENSE.txt) · [Güvenlik](SECURITY.md) · [Laboratuvar kullanımı](docs/laboratory-safety.md)
