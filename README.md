Conduit is a proof-of-concept tool designed to encode arbitrary data into video format, which enables the resulting video to be transffered via streaming platforms. While the main purpose of the tools is to transfer files from one device to another, it can theoretically be used as a storage medium. However this may or may not be wtihin the TOS of the related streaming platform, use the tool in this manner at your own discration.

Inspiration

Conduit is inspired from a youtube video that showed how to use discord as a storage medium, and was developed for 6 months in that direction, however due to changes in discord TOS the development has restarted in the current format. While Conduit is not the first tool that does data encoding into videos, its development process and the concept of data encoding into videos were my own idea after moving on from discord as the primary medium.

What Conduit does

Conduit takes an input file and produces a video 1.4 times the input file conatining the data of the input file in its frames. Resulting video (if properly compressed) will look like the black and white static of a television receivng no signal, this video then can be decoded back into the original file using the decoding functionaity. Conduit uses the png encoder as its video encoder allowing it to produce videos less than double the size (1.4 times) of the original file which puts it ahead of the similar tools which I came across.

Platform and Requirements

Developed on x64 Windows, probably won't run on Linux but try your luck.
Designed to be stand alone, no ffmpeg installation required.

Notes

Make sure to use .mov for the output format while encoding,

Don't use quotation around the input file path, only use characters accepted by fopen(),

Always decode into the format you encoded from i.e. if you encoded a .zip file decode into a .zip file,

If you can't playback the resulting .mov file use mpv->https://mpv.io
