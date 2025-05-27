![Conduit](https://github.com/user-attachments/assets/26b4f0b6-30eb-4d16-a0b8-a00d42190e83)


Conduit is a proof-of-concept tool designed to encode arbitrary data into video format, enabling the resulting video to be transferred via streaming platforms. While the primary purpose of the tool is to transfer files from one device to another, it can theoretically be used as a storage medium. However, this may or may not be within the Terms of Service (TOS) of the respective streaming platform—use the tool in this manner at your own discretion.

Inspiration

Conduit was inspired by a YouTube video demonstrating how to use Discord as a storage medium. Initially, development was focused in that direction for six months. However, due to changes in Discord’s TOS, development was restarted in its current format. While Conduit is not the first tool to encode data into video, the concept and development of encoding data into videos originated independently after shifting away from using Discord as the primary medium.
    
What Conduit Does

Conduit takes an input file and produces a video approximately 1.4 times the size of the original file, with the data embedded in its frames. The resulting video (if properly compressed) will resemble black-and-white static, similar to a television receiving no signal. This video can then be decoded back into the original file using Conduit's decoding functionality. Conduit uses a PNG-based video encoder, allowing it to produce videos that are significantly smaller (only 1.4 times the original file size) compared to similar tools, which often result in much larger output files.

Platform and Requirements

Developed on 64-bit Windows; it may not run on Linux, but you can try.  
Designed to be standalone—no FFmpeg installation required.  
Support for AVX512 instruction set.

Notes

Make sure to use the .mov format for output when encoding.  
Do not use quotation marks around the input file path; only use characters accepted by fopen().  
Always decode into the same format as the original file (e.g., if you encoded a .zip file, decode into a .zip file).  
If you can’t play back the resulting .mov file, use mpv: https://mpv.io
