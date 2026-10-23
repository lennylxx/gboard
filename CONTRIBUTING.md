# Contributing

[English](CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md)

## Copyright and legal principles

This project is an independent interoperability effort and is not
affiliated with Google. To limit copyright and trademark risk, every
contribution must follow the principles below. This is not legal advice.

### Do not commit Google files

1. Do not commit APKs, `.so` files, resources, dictionaries, HMM data
   packs, models, fonts, images, sounds, or any other file from Gboard.
   `.gitignore` already excludes `gboard_apk_source/` and `hmmoemdata/`.
2. Do not publish build artifacts (releases, installers, mirrors) that
   bundle any of these files.
3. Do not provide download links for the APK, or third-party mirrors or
   file-sharing links for the APK or data packs. Users must obtain the
   APK legally on their own and extract it locally with `setup.sh`.
   `setup.sh` downloads data packs only from Google's official public
   addresses (the same source the Gboard app uses); this project hosts
   no copies.

### Do not copy Google code or text

4. Do not commit decompiled output (jadx), and do not copy or
   line-by-line translate decompiled code into this project. Implement
   features independently from observed behavior and interfaces.
5. Interface-level facts may be referenced: JNI method names, class
   names, settings keys, data file names, and protobuf field numbers.
   They are required for interoperability.
6. Do not copy Google's UI text, `strings.xml` translations, prompt
   templates, or help documentation.
7. Do not paste large amounts of decompiled code or data file contents
   into issues, pull requests, or logs.

### Trademarks and branding

8. Do not use or imitate Google or Gboard logos, icons, or brand styling
   (for example, a "G"-like mark).
9. Use "Gboard" only to describe compatibility (for example, "compatible
   with the Gboard engine"), never as a product name, in icons, or in
   wording that implies an official relationship.
10. UI such as the candidate window may reference common color values,
    but its overall appearance must not suggest an official Google
    product.

### Do not circumvent protections

11. Do not bypass signature checks, license verification, DRM, or other
    technical protection measures.
12. Do not spoof device or account identity to access services that
    require Google authentication (such as AICore, Astrea, or GenAI
    gRPC).
13. Download data only from public addresses that require no
    authentication. Downloaded data stays on the user's machine and must
    not be redistributed.

### Other

14. Third-party open-source dependencies must follow their licenses and
    be credited in `NOTICE`.
15. Users are responsible for ensuring their use complies with the
    Gboard terms of service and local laws.
16. Maintainers will promptly review and remove content in response to
    notices from rights holders.
