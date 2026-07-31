# v5.15.36

- Added web hold-and-release controls for DAC1 and DAC2.
- Pointer release, cancel, lost capture, blur, page hide, visibility loss, and STOP ALL all issue DAC OFF safety commands.
- Deliberate Send, SendBLE, SendWiFi, SendUSB, SendUART, and SendSPP payloads are delivered as raw text with a newline, without event IDs, levels, source tags, request IDs, or bracketed debug wrappers.
- Send acknowledgements remain separate and concise.
