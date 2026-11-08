class Nero < Formula
  desc "Native C11 terminal and experimental network diagnostics"
  homepage "https://github.com/thekiran/nero"
  version "0.1.3"
  license "MIT"

  on_arm do
    url "https://github.com/thekiran/nero/releases/download/v0.1.3/nero-v0.1.3-macos-arm64.tar.gz"
    sha256 "e756c051891d9bf32abf75071c485510988ecbf196005dc5ab418b1104ac47e0"
  end
  on_intel do
    url "https://github.com/thekiran/nero/releases/download/v0.1.3/nero-v0.1.3-macos-x86_64.tar.gz"
    sha256 "8155b9b8b17789ebf9541cf7e9d71d256c97a49a9a03d5a1cf21583fda7a8171"
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
    assert_match "version v0.1.3", pipe_output("#{bin}/nero", "version\nexit\n", 0)
  end
end
