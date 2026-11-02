# Nero - SYN Flood Testing Tool

> **EXPERIMENTAL SOFTWARE — LABORATORY USE ONLY**
>
> Nero is an experimental network testing and research project. It is not intended for general public use, production deployment, or unauthorized network activity.
>
> All testing must be performed exclusively in isolated environments or on systems for which explicit authorization has been granted.
>
> This software is provided for controlled development, education, research, and testing purposes only.

> **DENEYSEL YAZILIM — YALNIZCA LABORATUVAR KULLANIMI**
>
> Nero, ağ testleri ve araştırmaları için geliştirilen deneysel bir projedir. Genel kullanıcı kullanımına, üretim ortamlarında dağıtıma veya izinsiz ağ faaliyetlerine yönelik değildir.
>
> Bütün testler yalnızca izole ortamlarda veya açıkça test yetkisi verilmiş sistemlerde gerçekleştirilmelidir.
>
> Proje yalnızca kontrollü geliştirme, eğitim, araştırma ve test amaçlarına yöneliktir.

## macOS development and safe validation

On macOS, `make` builds the experimental development CLI with the system C
compiler and SDK. No Homebrew paths or third-party libraries are needed.
Use `make CC=clang` to select Clang explicitly. No administrator privileges
or system-wide installation are needed for these development steps:

```sh
make
make test
make test-sanitize
make analyze
```

macOS supports the existing terminal commands, ANSI colors, ASCII graphics,
and in-memory history. The interface module uses `getifaddrs()`; its standalone
test harness lists local interfaces without probing the network. It is not a
new CLI command. Loopback is identified; other interfaces are labelled Unknown
because this API does not reliably distinguish Wi-Fi from Ethernet.

**`-Syn` is unavailable on macOS.** Its adapter reports that no network traffic
is generated. Neither the Linux load engine nor the port scanner is linked into
the macOS executable. This is intentionally partial development compatibility.

On Linux, the existing default source list and library dependencies are retained.
The tests build a separate executable with the same no-network adapter, so
`make test` and `make test-sanitize` never execute the Linux load engine.
Python 3 is required for tests; Clang is required for `make analyze`.
Existing Linux build/run/install instructions below are retained as historical
project documentation, not as a recommendation for public or production use.
Do not run the network engine as part of compatibility validation.

See [platform analysis and validation](docs/platform-compatibility.md) for scope,
known risks, test results, and untested functionality.


⚠️ **Legal Notice**  
This software is intended **only for educational, research, and testing purposes on your own systems**. Unauthorized use is illegal. The developer assumes no responsibility for misuse.

![2025-09-07 18-26-00](https://github.com/user-attachments/assets/270e73c3-a231-474a-a812-7a5dba635084)

---

## Overview
**Nero** is a high-performance network testing tool designed to simulate **SYN Flood attacks**.  

It allows you to:
- Assess the resilience of systems against SYN flood attacks  
- Test firewalls, IDS/IPS, and other security solutions  
- Provide realistic scenarios for network administrators and security researchers  

<div align="center">
  <img src="https://github.com/user-attachments/assets/60ef735b-c908-48c2-a8c1-99245a935e03" width="731" height="504" alt="Ekran görüntüsü 2025-09-07 142023" />
</div>

---
Install the required packages:

```bash
git clone https://github.com/thekiran/nero.git
sudo apt update
sudo apt install libcurl4-openssl-dev libcjson-dev libldns-dev libncurses-dev
cd nero
make
make install
sudo nero
```


<div align="center">
  <a href="https://www.youtube.com/watch?v=2HGtMkmqwpU">
    <img src="https://img.youtube.com/vi/2HGtMkmqwpU/0.jpg" width="731" height="504" alt="Project Demo Video" />
  </a>
</div>
