# Geliştirme ve doğrulama

[Proje](../README.md) · [Kod haritası](code-map.md) · [Belgeler](README.md)

Tüm komutları depo kökünde çalıştırın.

## Make

| Komut | Sonuç |
| --- | --- |
| `make` | Platforma göre `nero` oluşturur |
| `make test` | 9 temel ve 19 terminal regresyon testini çalıştırır |
| `make test-sanitize` | Aynı çevrimdışı testleri ASan/UBSan ile çalıştırır |
| `make analyze` | CLI ve seçilen macOS adaptörünü Clang ile analiz eder |
| `make help` | Kullanılabilir geliştirme komutlarını gösterir |
| `make clean` | Üretilen uygulamayı ve `.build/` dizinini temizler |

Gereksinimler: C11 derleyicisi, Make ve testler için Python 3.
macOS için Xcode Command Line Tools / SDK yeterlidir.
Linux'un mevcut Make hedefi ayrıca curl, cJSON, ldns ve ncurses geliştirme
kitaplıklarını kullanır; çevrimdışı testler ağ modüllerini bağlamaz.

## CMake

CMake 3.21+ gerekir. Platformunuza göre `macos`, `linux` veya `windows` preset'ini seçin.

```sh
cmake --preset macos
cmake --build --preset macos
ctest --preset macos
```

POSIX sanitizer preset'leri `macos-sanitize` ve `linux-sanitize` adlarını taşır.
Windows için Visual Studio 2022 C araçları gerekir; yalnızca platform temel
bileşenleri derlenir. Windows CLI desteği yoktur.

## Çıktılar

```text
.build/
├── obj/          Make nesneleri ve bağımlılık dosyaları
├── tests/        Çevrimdışı test uygulamaları
├── sanitize/     Sanitizer test uygulamaları
├── analysis/     Clang analiz raporları
└── cmake/        Platform preset'lerinin derleme dizinleri
```

Kökteki `nero` ve `.build/` Git dışında kalır. Kaynak dizinlerine derleme çıktısı eklemeyin.

## Kod biçimi

`.editorconfig` girinti ve satır sonlarını, `.clang-format` C biçimini tanımlar.
CLI kodunda dört boşluk, 100 sütun ve ayrı satırda fonksiyon açılış süslü parantezi
kullanılır. `clang-format` geliştirme aracıdır; derleme için gerekli değildir.

```sh
clang-format --dry-run --Werror src/cli/terminal.c src/cli/input.c \
  src/cli/render.c src/cli/commands.c src/cli/ascii.c src/cli/cli_internal.h
```

Üçüncü taraf kodunu veya eski modülleri topluca biçimlendirmeyin.
İşlevsel değişiklikleri biçim değişikliklerinden ayrı inceleyin.

## Bütünlük kayıtları

`baseline.json` tarihsel durumu, `layout-baseline.json` dosya taşıma durumunu korur.
`readability-baseline.json` CLI'nin sorumluluklara ayrılması ve derleme düzeni için
önceki/sonraki hash'leri ve yeni dosyaları kaydeder. Test, bu zinciri ve mevcut
kaynakları doğrular. Yeni bir değişiklikte kayıtları otomatik yenileyerek hatayı
örtmeyin; farkı inceleyin, ilgili davranış testlerini çalıştırın ve nedeni kaydedin.
