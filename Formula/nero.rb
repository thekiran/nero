class Nero < Formula
  desc "Native C11 terminal and experimental network diagnostics"
  homepage "https://github.com/thekiran/nero"
  version "0.1.4"
  license "MIT"

  on_arm do
    url "https://github.com/thekiran/nero/releases/download/v0.1.4/nero-v0.1.4-macos-arm64.tar.gz"
    sha256 "ac00632f8217067f81db72bb7f9f6152670f911b7bf8fc2087f5aed18323dbf2"
  end
  on_intel do
    url "https://github.com/thekiran/nero/releases/download/v0.1.4/nero-v0.1.4-macos-x86_64.tar.gz"
    sha256 "c176260d757be79935895d884545ffbb8aefeb3f960e90be662c7f267279e108"
  end

  depends_on :macos
  on_macos do
    depends_on macos: :sequoia
  end

  def install
    bin.install "bin/nero"
    doc.install "README.md", "INSTALL.md", "BUILD-INFO.json"
  end

  test do
    assert_match "version v0.1.4", pipe_output("#{bin}/nero", "version\nexit\n", 0)
  end
end
