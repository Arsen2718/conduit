#include <gd.h>
#include <io.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <threads.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
}

#define BUFFER_SIZE 6*1024*1024
#define FRAME_RATE 60
#define WIDTH 3840  
#define HEIGHT 2160
#define WRITEWINDOWSIZE 3 //3 recommended, currently not used
#define READWINDOWSIZE 10

typedef AVPacket* AVPacketPtr;

int PIXELS[8][6][2] = { {{0,0},{4,0},{8,0},{12,0},{16,0},{20,0}},
                        {{0,4},{4,4},{8,4},{12,4},{16,4},{20,4}},
                        {{0,8},{4,8},{8,8},{12,8},{16,8},{20,8}},
                        {{0,12},{4,12},{8,12},{12,12},{16,12},{20,12}},
                        {{0,16},{4,16},{8,16},{12,16},{16,16},{20,16}},
                        {{0,20},{4,20},{8,20},{12,20},{16,20},{20,20}},
                        {{0,24},{4,24},{8,24},{12,24},{16,24},{20,24}},
                        {{0,28},{4,28},{8,28},{12,28},{16,28},{20,28}}
};

typedef struct bytes_6t {
    unsigned char bytes[6];
} bytes_6;

typedef struct threadparamspipe_t {

    char* invideodir = NULL;
    char* outfiledir = NULL;
    atomic_int shared_value = 0;
    atomic_bool has_ended = false;
    AVPacketPtr* pnglist_p = NULL;

}writebackparams;

typedef struct pageproccessorparams_t {

    AVPacket* packet;
    size_t page;
    char* returningfiledir;
    size_t pos;
    atomic_int* win_size;
    thrd_t* inthread;

}pageproccessorparams;

typedef struct readtoparams_t {

    char* infiledir = NULL;
    char* outvideodir = NULL;
    int width = 3840;
    int height = 2160;
    int BLOCK_WIDTH = 24;
    int BLOCK_HEIGHT = 32;
    int X_BLOCKS = 160;
    int Y_BLOCKS = 67;
    size_t READ_BUF_SIZE = X_BLOCKS * Y_BLOCKS;
    gdImagePtr* GDarray = NULL;
    atomic_int count = 0;
    atomic_bool* pushlist = NULL;
    AVPacketPtr* AVarray = NULL;
    atomic_bool* pushlistpacket = NULL;
    atomic_int failcount = 0;
    size_t numberofpages = 0;
    size_t numberofpagesbeforepad = 0;

}readtoparams;

typedef struct packetparams_t {

    readtoparams* sharedparams;
    int packetno;
    AVPacket* return_val;
    thrd_t* inthrd;
    atomic_int* count;

}packetparams;

typedef struct readtoparamsthread_t {

    thrd_t* inthread;
    readtoparams* params;
    atomic_int* window_size;
    size_t pos;
    size_t eof_pos;

}readtoparamsthread;

bytes_6 decode_blocks_by_offset(gdImagePtr block, int offset_x, int offset_y, size_t page);
size_t freadbch(void* buffer, size_t element_size, size_t element_count, FILE* stream);
int64_t probevideo(char* invideodir);
void menu();

void readpagestoram(readtoparams& params);
void proccesspagereader(readtoparams& params, atomic_int& window_size, size_t pos, size_t eof_pos);


void pushpagestoram(char* invideodir, char* returningfiledir, AVPacketPtr* pnglist_p, atomic_int& count, atomic_bool& hasended_p);

void writefromrampagesthreaded(writebackparams* params);
void proccesspage(AVPacket*& packet, size_t page, char* returningfiledir, size_t pos, atomic_int& win_size);
int proccesspagecaller(void* args);
int writefromrampagesthreadedcaller(void* args);

int pushpagestoramcaller(void* args);
void pipelinedramwriteback(void* args);
int processpagereadercaller(void* args);
int readpagestoramcaller(void* args);
void pipelinedramvideocreate(void* args);
void packetcreatornoenc(packetparams& params);
int packetcreatornoenccaller(void* args);
void createvideofromrampagesthreaded(readtoparams& params);
int createvideofromrampagesthreadedcaller(void* args);
int createvideocaller(void* args);
void createvideo(readtoparams& params);

int main() {

    menu();

    return 0;

}

void menu() {

    bool exit = false;
    int choice = 0;

    do {
        char out[256]; char in[256];
        printf("Encode (1), Decode (2), Exit(3)\n");
        int capture = scanf("%d", &choice); capture = getchar();

        if (choice == 1) {

            printf("Enter source file path (max 255 chars)\n");
            fgets(in, 255, stdin); in[strcspn(in, "\n")] = '\0';
            printf("Enter output video path (path must end with .mov, max 255 chars, !no pqges)\n");
            fgets(out, 255, stdin); out[strcspn(out, "\n")] = '\0';

            readtoparams encode; encode.infiledir = in; encode.outvideodir = out;
            pipelinedramvideocreate(&encode);
        }

        else if (choice == 2) {

            printf("Enter encoded video path (max 255 chars)\n");
            fgets(in, 255, stdin); in[strcspn(in, "\n")] = '\0';
            printf("Enter returning file path (make sure to use the correct container, max 255 chars)\n");
            fgets(out, 255, stdin); out[strcspn(out, "\n")] = '\0';

            writebackparams decode; decode.invideodir = in; decode.outfiledir = out;
            pipelinedramwriteback(&decode);
        }

        else if (choice == 3) {

            exit = true;
        }

        else {

            printf("Enter a valid choice\n");
        }

    } while (!exit);

}


void readpagestoram(readtoparams& params) {    
    
    atomic_int window_size = 0;
    readtoparamsthread* outthread;

    FILE* probe = fopen(params.infiledir, "rb+");
    fseek(probe, 0, SEEK_END);
    size_t eof_pos = _ftelli64(probe);
    fclose(probe);

    size_t k = 0;

    for (; ;) {

        if (window_size < READWINDOWSIZE && k < params.numberofpages) {

            outthread = new readtoparamsthread;

            outthread->inthread = new thrd_t;
            outthread->params = &params;
            outthread->pos = k * 10720 * 6;
            outthread->window_size = &window_size;
            outthread->eof_pos = eof_pos;

            window_size.fetch_add(1);

            //params.gdvector.push_back(NULL);

            thrd_create(outthread->inthread, processpagereadercaller, outthread);
            thrd_detach(*(outthread->inthread));

            k++;
        }

        else if (k == params.numberofpages)
            break;
    }

    while (window_size != 0) {}

    //printf("%d", params.count.load());    

    /*size_t padding = 0; putchar('\n');

    if (params.count % 60 != 0) {

        printf("Applying padding to match a multiple of 60 + 1 frames.\n");
        printf("Currently padded 0 frames.\n");

        while (params.count % 60 != 0) {

            gdImagePtr img = gdImageCreate(params.width, params.height);
            int black = gdImageColorAllocate(img, 0, 0, 0);
            gdImageFilledRectangle(img, 0, 0, params.width - 1, params.height - 1, black);
            params.gdvector[k] = img;
            params.pushlist[k] = true;
            printf("\033[F\033[K");
            params.count.fetch_add(1);
            k++;
            printf("Currently padded %zd frames.\n", ++padding);
        }
    }

    gdImagePtr img = gdImageCreate(params.width, params.height);
    int black = gdImageColorAllocate(img, 0, 0, 0);
    gdImageFilledRectangle(img, 0, 0, params.width - 1, params.height - 1, black);
    params.gdvector[k] = img;
    params.pushlist[k] = true;
    printf("\033[F\033[K");
    params.count.fetch_add(1);
    k++;
    printf("Currently padded %zd frames.\n", ++padding);
    printf("Padding operation successful.\n");
    
    printf("\nDone. Total frames: %zu\n", (size_t)params.count); */

    //params.has_really_ended = true;

    return;
}
void proccesspagereader(readtoparams& params, atomic_int& window_size, size_t pos,size_t eof_pos) {
        
    if (pos / 64320 > params.numberofpagesbeforepad) {

        gdImagePtr img = gdImageCreate(params.width, params.height);
        int black = gdImageColorAllocate(img, 0, 0, 0);
        gdImageFilledRectangle(img, 0, 0, params.width - 1, params.height - 1, black);
        params.GDarray[pos / 64320] = img;
        params.pushlist[pos / 64320].store(true);
        params.count.fetch_add(1);
        window_size.fetch_sub(1);        
    }
    
    else {

        FILE* infile = fopen(params.infiledir, "rb+");
        if (!infile) {
            perror("Error can't open file\n");
            exit(1);
        }

        setvbuf(infile, NULL, _IOFBF, (size_t)(1) << 20);
        _fseeki64(infile, pos, SEEK_SET);

        bytes_6* buffer = new bytes_6[params.READ_BUF_SIZE];
        int elements_read = 0;

        elements_read = (int)fread(buffer, sizeof(char), sizeof(bytes_6) * params.READ_BUF_SIZE, infile);

        gdImagePtr img = gdImageCreate(params.width, params.height);
        const int black = gdImageColorAllocate(img, 0, 0, 0);
        const int white = gdImageColorAllocate(img, 255, 255, 255);
        gdImageFilledRectangle(img, 0, 0, params.width - 1, params.height - 1, black);

        int l;

        for (l = 0; l < (elements_read / 6); l++) {
            const int grid_x = l / (params.X_BLOCKS);  // Calculate row position
            const int grid_y = l % (params.X_BLOCKS);  // Calculate column position
            const int base_x = grid_y * (params.BLOCK_WIDTH);
            const int base_y = grid_x * (params.BLOCK_HEIGHT);

            const bytes_6* current = &buffer[l];
            for (int j = 0; j < 6; j++) {
                unsigned char mask = 0x80;
                for (int i = 0; i < 8; i++) {
                    if (current->bytes[j] & mask) {
                        const int px = base_x + PIXELS[i][j][0];
                        const int py = base_y + PIXELS[i][j][1];
                        gdImageFilledRectangle(img, px, py, px + 3, py + 3, white);
                    }
                    mask >>= 1;
                }
            }
        }

        if (elements_read % 6 != 0 && feof(infile)) { //last part overflows to new chunk

            const int grid_x = l / (params.X_BLOCKS);  // Calculate row position
            const int grid_y = l % (params.X_BLOCKS);  // Calculate column position
            const int base_x = grid_y * (params.BLOCK_WIDTH);
            const int base_y = grid_x * (params.BLOCK_HEIGHT);

            const bytes_6* current = &buffer[l]; int j;
            for (j = 0; j < elements_read % 6; j++) {
                unsigned char mask = 0x80;
                for (int i = 0; i < 8; i++) {
                    if (current->bytes[j] & mask) {
                        const int px = base_x + PIXELS[i][j][0];
                        const int py = base_y + PIXELS[i][j][1];
                        gdImageFilledRectangle(img, px, py, px + 3, py + 3, white);
                    }
                    mask >>= 1;
                }
            }

            for (int i = 0; i < 8; i++) {

                const int px = base_x + PIXELS[i][j][0];
                const int py = base_y + PIXELS[i][j][1];
                gdImageFilledRectangle(img, px, py, px + 3, py + 3, white);
            }
        }

        else if (feof(infile)) { //last part doesnt overflow

            const int grid_x = l / (params.X_BLOCKS);  // Calculate row position
            const int grid_y = l % (params.X_BLOCKS);  // Calculate column position
            const int base_x = grid_y * (params.BLOCK_WIDTH);
            const int base_y = grid_x * (params.BLOCK_HEIGHT);

            for (int i = 0; i < 8; i++) {

                const int px = base_x + PIXELS[i][0][0];
                const int py = base_y + PIXELS[i][0][1];
                gdImageFilledRectangle(img, px, py, px + 3, py + 3, white);
            }
        }

        if (feof(infile)) { //hint for switching to searching in byte range

            l++;

            const int grid_x = l / (params.X_BLOCKS);  // Calculate row position
            const int grid_y = l % (params.X_BLOCKS);  // Calculate column position
            const int base_x = grid_y * (params.BLOCK_WIDTH);
            const int base_y = grid_x * (params.BLOCK_HEIGHT);

            for (int j = 0; j < 6; j++) {
                for (int i = 0; i < 8; i++) {

                    const int px = base_x + PIXELS[i][j][0];
                    const int py = base_y + PIXELS[i][j][1];
                    gdImageFilledRectangle(img, px, py, px + 3, py + 3, white);
                }
            }
        }

        params.GDarray[pos / 64320] = img;
        params.pushlist[pos / 64320].store(true);
        params.count.fetch_add(1);
        delete[] buffer;
        fclose(infile);
        window_size.fetch_sub(1);
    }
}
int processpagereadercaller(void* args) {

    readtoparamsthread* params = (readtoparamsthread*)args;
    proccesspagereader(*(params->params), *(params->window_size), params->pos, params->eof_pos);

    delete params->inthread;
    delete params;

    return 0;
}
int readpagestoramcaller(void* args) {

    readtoparams* params = (readtoparams*)args;
    readpagestoram(*params);

    return 0;
}
void packetcreatornoenc(packetparams& params) {

    AVPacket* packet = av_packet_alloc();
    int filesize;

    // Allocate frame
    AVRational avg_frame_rate = av_make_q(60, 1);
    AVRational time_base = av_make_q(1, 15360);
    int64_t packet_duration = (int64_t)time_base.den / time_base.num / avg_frame_rate.num * avg_frame_rate.den;

    while (!(params.sharedparams->pushlist[params.packetno])) { timespec x = { 0, 1000000 }; thrd_sleep(&x, NULL); /*printf("count:%d z:%d\n", params.count.load(), z); */ }
    
    // Encode frame    

    uint8_t* img = (uint8_t*)gdImagePngPtr(params.sharedparams->GDarray[params.packetno], &filesize);
    av_new_packet(packet, filesize);
    packet->duration = packet_duration;
    packet->stream_index = 0;
    packet->time_base = av_make_q(1, 15360);
    packet->pts = (int64_t)(params.packetno) * packet_duration;
    packet->dts = packet->pts;
    packet->size = filesize;
    memcpy(packet->data, img, filesize);
    gdFree(img);
    gdImageDestroy(params.sharedparams->GDarray[params.packetno]);
    params.sharedparams->GDarray[params.packetno] = NULL;

    params.sharedparams->AVarray[params.packetno] = packet;
    params.sharedparams->pushlistpacket[params.packetno].store(true);

    params.count->fetch_sub(1);

    return;

}
int packetcreatornoenccaller(void* args) {

    packetparams* params = (packetparams*)args;
    packetcreatornoenc(*params);

    delete params->inthrd;
    params->sharedparams = NULL;
    delete params;

    return 0;
}
int createvideocaller(void* args) {

    readtoparams* params = (readtoparams*)args;
    createvideo(*params);

    return 0;
}
void createvideo(readtoparams& params) {

    AVFormatContext* formatContext = NULL;
    const AVOutputFormat* outputFormat = NULL;
    AVCodecContext* codecContext = NULL;
    const AVCodec* codec = NULL;
    AVStream* videoStream = NULL;

    int ret;

    // Allocate format context
    avformat_alloc_output_context2(&formatContext, NULL, NULL, params.outvideodir);
    if (!formatContext) {
        fprintf(stderr, "Could not allocate format context\n");
        return;
    }
    outputFormat = formatContext->oformat;

    // Find PNG encoder
    codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        return;
    }

    // Create video stream
    videoStream = avformat_new_stream(formatContext, NULL);
    if (!videoStream) {
        fprintf(stderr, "Could not create stream\n");
        return;
    }
    videoStream->id = formatContext->nb_streams - 1;

    // Allocate codec context
    codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        fprintf(stderr, "Could not allocate codec context\n");
        return;
    }

    // Set codec parameters
    codecContext->codec_id = AV_CODEC_ID_PNG;
    codecContext->width = WIDTH;
    codecContext->height = HEIGHT;
    codecContext->pix_fmt = AV_PIX_FMT_MONOBLACK;
    codecContext->field_order = AV_FIELD_PROGRESSIVE;
    codecContext->time_base = av_make_q(1, 15360);
    codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
    codecContext->sample_aspect_ratio = { 1,1 };
    videoStream->sample_aspect_ratio = codecContext->sample_aspect_ratio;

    // Open codec
    if ((ret = avcodec_open2(codecContext, codec, NULL)) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return;
    }

    // Set stream codec parameters
    videoStream->time_base = codecContext->time_base;
    avcodec_parameters_from_context(videoStream->codecpar, codecContext);

    // Open output file
    if (!(outputFormat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&formatContext->pb, params.outvideodir, AVIO_FLAG_WRITE)) < 0) {
            fprintf(stderr, "Could not open output file\n");
            return;
        }
    }

    av_dict_set(&videoStream->metadata, "encoder", "Lavc61.3.100 png", 0);

    // Write header
    if ((ret = avformat_write_header(formatContext, NULL)) < 0) {
        fprintf(stderr, "Error writing header\n");
        return;
    }

    // Allocate frame
    AVRational avg_frame_rate = av_make_q(60, 1);
    timespec x = { 0, 1000000 };

    for (int z = 0; ;) {

        if (z == params.numberofpages) {

            printf("Video loop broken!! z = %d\n", z);

            break;
        }

        else if (params.pushlistpacket[z]) {

            av_interleaved_write_frame(formatContext, params.AVarray[z]);
            gdFree(params.AVarray[z]->data);
            av_packet_unref(params.AVarray[z]);            
            av_packet_free(&(params.AVarray[z]));
            z++;
        }

        thrd_sleep(&x, NULL);
    }

    // Write trailer
    av_write_trailer(formatContext);

    // Cleanup
    avcodec_free_context(&codecContext);

    avio_close(formatContext->pb);
    avformat_free_context(formatContext);

    //avio_close(formatContext->pb);

    return;
}
void createvideofromrampagesthreaded(readtoparams& params) {
            
    packetparams* packetthread;
    atomic_int count = 0;

    // Encode frames
    for (int z = 0; ; ) {

        if (z < params.count) {
            
            packetthread = new packetparams;            
            packetthread->packetno = z;
            packetthread->sharedparams = &params;
            packetthread->count = &count;
            packetthread->inthrd = new thrd_t; 

            //params.AVvector.push_back(NULL);

            thrd_create(packetthread->inthrd, packetcreatornoenccaller, packetthread);
            thrd_detach(*packetthread->inthrd);

            count.fetch_add(1);

            z++;
        }

        else if (z == params.numberofpages) {
                        
            break;
        }
    }

    while (count.load() != 0) {
        timespec x = { 0, 1000000 }; thrd_sleep(&x, NULL);
    }

    return;
}
int createvideofromrampagesthreadedcaller(void* args) {

    readtoparams* params = (readtoparams*)args;
    createvideofromrampagesthreaded(*params);
    return 0;
}

void pipelinedramvideocreate(void* args) {

    thrd_t thrd1, thrd2, thrd3;
    
    readtoparams* params = (readtoparams*)args;
    FILE* test = fopen(params->infiledir, "rb+");    
    fseek(test, 0, SEEK_END);
    int64_t size = _ftelli64(test);
    fclose(test);
    int64_t numberofpages = size / 64320;

    if (size % 64320 == 0)
        numberofpages++;

    params->numberofpagesbeforepad = numberofpages;

    if (numberofpages % 60 != 0) {
        while (numberofpages % 60 != 0)
            numberofpages++;
    }
    numberofpages++;

    params->numberofpages = numberofpages;

    printf("Total pages = %zu\n", numberofpages);

    params->AVarray = new AVPacketPtr[numberofpages];
    params->GDarray = new gdImagePtr[numberofpages];

    atomic_bool* pushlist = new atomic_bool[numberofpages];
    params->pushlist = pushlist;
    atomic_bool* pushlist2 = new atomic_bool[numberofpages];
    params->pushlistpacket = pushlist2;
    for (int i = 0; i < numberofpages; i++) { pushlist[i].store(false); }
    for (int i = 0; i < numberofpages; i++) { pushlist2[i].store(false); }
    memset(params->AVarray, NULL, sizeof(AVPacketPtr) * numberofpages);
    memset(params->GDarray, NULL, sizeof(gdImagePtr) * numberofpages);


    thrd_create(&thrd1, readpagestoramcaller, args);
    thrd_create(&thrd2, createvideofromrampagesthreadedcaller, args);
    thrd_create(&thrd3, createvideocaller, args);
    thrd_join(thrd1, NULL);
    printf("first thread joined\n");
    thrd_join(thrd2, NULL);
    printf("second thread joined\n");
    thrd_join(thrd3, NULL);
    printf("third thread joined\n");


    delete[] pushlist;
    delete[] pushlist2;
    delete[] params->AVarray;
    delete[] params->GDarray;
}


void pushpagestoram(char* invideodir, char* returningfiledir, AVPacketPtr* pnglist_p,atomic_int& count, atomic_bool& hasended_p) {

    // Initialize all pointers to NULL
    AVFormatContext* fmt_ctx = NULL;
    AVCodecContext* dec_ctx = NULL, * enc_ctx = NULL;
    const AVCodec* dec_codec = NULL, * enc_codec = NULL;
    AVFilterGraph* filter_graph = NULL;
    AVFilterContext* buffer_src_ctx = NULL, * buffer_sink_ctx = NULL;
    AVFrame* frame = NULL, * mono_frame = NULL;
    AVPacket* pkt = NULL;
    AVFilterInOut* outputs = NULL;
    AVFilterInOut* inputs = NULL;
    AVCodecParameters* codecpar = NULL;
    int video_stream_idx = -1;
    int ret = 0;
    int frame_count = 0;
    const char* filter_descr = "lutyuv=y='if(gte(val,128),255,0)',format=monob";
    AVPacket* result = NULL;

    // Open input file
    ret = avformat_open_input(&fmt_ctx, invideodir, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open input file\n");
        goto final_cleanup;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream information\n");
        goto final_cleanup;
    }

    // Find video stream
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }
    if (video_stream_idx == -1) {
        fprintf(stderr, "No video stream found\n");
        ret = AVERROR(EINVAL);
        goto final_cleanup;
    }

    // Initialize decoder
    codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
    dec_codec = avcodec_find_decoder(codecpar->codec_id);
    if (!dec_codec) {
        fprintf(stderr, "Unsupported codec\n");
        ret = AVERROR(EINVAL);
        goto final_cleanup;
    }

    dec_ctx = avcodec_alloc_context3(dec_codec);
    if (!dec_ctx) {
        fprintf(stderr, "Could not allocate decoder context\n");
        ret = AVERROR(ENOMEM);
        goto final_cleanup;
    }

    dec_ctx->time_base = fmt_ctx->streams[video_stream_idx]->time_base;
    dec_ctx->thread_count = 9;
    dec_ctx->thread_type = FF_THREAD_SLICE;

    ret = avcodec_parameters_to_context(dec_ctx, codecpar);

    if (ret < 0) {
        fprintf(stderr, "Could not copy codec parameters\n");
        goto final_cleanup;
    }

    ret = avcodec_open2(dec_ctx, dec_codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open decoder\n");
        goto final_cleanup;
    }

    // Create filter graph
    filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
        fprintf(stderr, "Could not allocate filter graph\n");
        ret = AVERROR(ENOMEM);
        goto final_cleanup;
    }

    filter_graph->thread_type = AVFILTER_THREAD_SLICE;
    filter_graph->nb_threads = 9;

    char args[512];
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d", dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt, dec_ctx->time_base.num, dec_ctx->time_base.den);

    // Create buffer source
    ret = avfilter_graph_create_filter(&buffer_src_ctx, avfilter_get_by_name("buffer"), "in", args, NULL, filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Cannot create buffer source\n");
        goto final_cleanup;
    }

    // Create buffer sink
    ret = avfilter_graph_create_filter(&buffer_sink_ctx,
        avfilter_get_by_name("buffersink"),
        "out", NULL, NULL, filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Cannot create buffer sink\n");
        goto final_cleanup;
    }

    // Parse filter graph
    outputs = avfilter_inout_alloc();
    inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = buffer_src_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = buffer_sink_ctx;
    inputs->pad_idx = 0;
    inputs->next = NULL;

    ret = avfilter_graph_parse_ptr(filter_graph, filter_descr, &inputs, &outputs, NULL);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret < 0) {
        fprintf(stderr, "Failed to parse filter graph\n");
        goto final_cleanup;
    }

    ret = avfilter_graph_config(filter_graph, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to configure filter graph\n");
        goto final_cleanup;
    }

    // Initialize encoder
    enc_codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!enc_codec) {
        fprintf(stderr, "PNG encoder not found\n");
        ret = AVERROR(EINVAL);
        goto final_cleanup;
    }

    enc_ctx = avcodec_alloc_context3(enc_codec);
    if (!enc_ctx) {
        fprintf(stderr, "Could not allocate encoder context\n");
        ret = AVERROR(ENOMEM);
        goto final_cleanup;
    }

    enc_ctx->width = dec_ctx->width;
    enc_ctx->height = dec_ctx->height;
    enc_ctx->pix_fmt = AV_PIX_FMT_MONOBLACK;
    enc_ctx->time_base = { 1, 25 };
    enc_ctx->thread_type = FF_THREAD_FRAME;
    enc_ctx->thread_count = 9;

    ret = avcodec_open2(enc_ctx, enc_codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Cannot open PNG encoder\n");
        goto final_cleanup;
    }

    // Allocate frames
    frame = av_frame_alloc();
    mono_frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !mono_frame || !pkt) {
        fprintf(stderr, "Could not allocate frames/packet\n");
        ret = AVERROR(ENOMEM);
        goto final_cleanup;
    }

    ret = av_read_frame(fmt_ctx, pkt);

    // Processing loop
    while (!ret) {

        if (pkt->stream_index == video_stream_idx) {
            ret = avcodec_send_packet(dec_ctx, pkt);

            while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    fprintf(stderr, "Error decoding frame\n");
                    break;
                }

                // Apply filters
                ret = av_buffersrc_add_frame(buffer_src_ctx, frame);
                av_frame_unref(frame);
                if (ret < 0) {
                    fprintf(stderr, "Error feeding filter graph\n");
                    break;
                }

                while (1) {
                    ret = av_buffersink_get_frame(buffer_sink_ctx, mono_frame);
                    if (ret == AVERROR(EAGAIN)) break;
                    if (ret == AVERROR_EOF) break;
                    if (ret < 0) {
                        fprintf(stderr, "Error getting filtered frame\n");
                        break;
                    }

                    ret = avcodec_send_frame(enc_ctx, mono_frame);
                    av_frame_unref(mono_frame);
                    if (ret < 0) {
                        fprintf(stderr, "Error sending frame to encoder\n");
                        break;
                    }

                    while (ret >= 0) {
                        ret = avcodec_receive_packet(enc_ctx, pkt);
                        if (ret == AVERROR(EAGAIN)) break;
                        if (ret == AVERROR_EOF) break;
                        if (ret < 0) {
                            fprintf(stderr, "Error encoding frame\n");
                            break;
                        }

                        result = av_packet_clone(pkt);
                        pnglist_p[frame_count] = result;

                        //if (!(frame_count % 100)) {
                        //
                        //  printf("%d\n", frame_count);
                        //}

                        frame_count++;
                        count.fetch_add(1);
                    }
                }
            }
        }
        av_packet_unref(pkt);
        ret = av_read_frame(fmt_ctx, pkt);
    }

    // Flush decoder
    avcodec_send_packet(dec_ctx, NULL);
    while (1) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) {
            fprintf(stderr, "Error flushing decoder\n");
            break;
        }

        // Process remaining frames
        ret = av_buffersrc_add_frame(buffer_src_ctx, frame);
        av_frame_unref(frame);
        if (ret < 0) {
            fprintf(stderr, "Error feeding filter graph during flush\n");
            break;
        }

        while (1) {
            ret = av_buffersink_get_frame(buffer_sink_ctx, mono_frame);
            if (ret == AVERROR(EAGAIN)) break;
            if (ret == AVERROR_EOF) break;
            if (ret < 0) {
                fprintf(stderr, "Error getting filtered frame during flush\n");
                break;
            }

            // Encode frame
            ret = avcodec_send_frame(enc_ctx, mono_frame);
            av_frame_unref(mono_frame);
            if (ret < 0) {
                fprintf(stderr, "Error sending frame to encoder during flush\n");
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_packet(enc_ctx, pkt);
                if (ret == AVERROR(EAGAIN)) break;
                if (ret == AVERROR_EOF) break;
                if (ret < 0) {
                    fprintf(stderr, "Error encoding frame during flush\n");
                    break;
                }

                //AVPacket* result = NULL;
                result = av_packet_clone(pkt);
                pnglist_p[frame_count] = result;

                if (!(frame_count % 100)) {

                    printf("%d\n", frame_count);
                }

                frame_count++;
                count.fetch_add(1);
            }
        }
    }

    //flush encoder
    ret = avcodec_send_frame(enc_ctx, NULL);
    while (ret >= 0) {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            fprintf(stderr, "Error flushing encoder\n");
            break;
        }

        //AVPacket* result = NULL;
        result = av_packet_clone(pkt);
        pnglist_p[frame_count] = result;

        if (!(frame_count % 100)) {

            printf("%d\n", frame_count);
        }

        frame_count++;
        count.fetch_add(1);
    }

    hasended_p.store(true);

final_cleanup:
    avformat_close_input(&fmt_ctx);
    avcodec_free_context(&dec_ctx);
    avcodec_free_context(&enc_ctx);
    avfilter_graph_free(&filter_graph);
    av_frame_free(&frame);
    av_frame_free(&mono_frame);
    av_packet_free(&pkt);

    return;
}
void writefromrampagesthreaded(writebackparams* params) {
        
    atomic_int windowsize = 0;
    FILE* temp = fopen(params->outfiledir, "wb+");
    fclose(temp);

    pageproccessorparams* outthread;

    for (size_t k = 0; ;) {

        if (k < params->shared_value) {
            
            outthread = new pageproccessorparams;   

            outthread->inthread = new thrd_t;
            outthread->packet = params->pnglist_p[k];
            outthread->page = k;
            outthread->pos = k * 10720 * 6;
            outthread->returningfiledir = params->outfiledir;
            outthread->win_size = &windowsize;
            
            //params->pnglist_p.pop_front();
            windowsize.fetch_add(1);
            thrd_create(outthread->inthread, proccesspagecaller, outthread);
            thrd_detach(*(outthread->inthread));
            k++;

        }

        else if (k == params->shared_value && params->has_ended)
            break;

        /*else {
        
          printf("k = %zu, hasended= %d, windowsize= %d\n", k, params->has_ended.load(), windowsize.load()); //debug needed here since sometimes this thread gets clogged

        }*/
    }

    while (windowsize != 0) {}

    //Start truncation

    FILE* myreturn = fopen(params->outfiledir, "rb+");

    fseek(myreturn, 0, SEEK_END);
    bytes_6 test_bytes;
    char test_byte;
    int breaker = 0;

    while (1) {

        freadbch(&test_bytes, sizeof(bytes_6), 1, myreturn);
        //printf("%zu \n", _ftelli64(myreturn));
        for (int i = 0; i < 6; i++) {
            if (test_bytes.bytes[i] == 0) {

                continue;
            }
            else {

                breaker = 1;
            }
        }
        if (breaker)
            break;
    }

    while (1) {

        int key_count = 0;
        freadbch(&test_byte, sizeof(char), 1, myreturn);
        //printf("%zu \n", _ftelli64(myreturn));
        unsigned char mask = 0b10000000;
        for (int i = 0; i < 8; i++) {
            if (test_byte & mask) {

                key_count++;
                mask = mask >> 1;
            }
        }

        if (key_count == 8) {

            break;
        }
    }

    if (_chsize_s(_fileno(myreturn), _ftelli64(myreturn)) != 0) {
        perror("_chsize_s");
        fclose(myreturn);
        return;
    }

    fclose(myreturn);
    return;
}
void proccesspage(AVPacket*& packet, size_t page, char* returningfiledir, size_t pos,atomic_int& win_size) {

    gdImagePtr current_image = gdImageCreateFromPngPtr(packet->size, packet->data);
    if (!current_image) { exit(1); }
    bytes_6 decoded;
    FILE* returning; while(!(returning = fopen(returningfiledir, "rb+"))) { printf("clog!\n"); }
    _fseeki64(returning, pos, SEEK_SET);
    bytes_6* buffer = new bytes_6[10720];

    // Process all blocks in the image
    for (int i = 0; i < 67; i++) {
        int y = 32 * i;
        for (int j = 0; j < 160; j++) {
            int x = 24 * j;
            
            decoded = decode_blocks_by_offset(current_image, x, y, page);
            memcpy(&(buffer[160 * i + j]), &decoded, sizeof(bytes_6));
        }
    }

    fwrite(buffer, sizeof(bytes_6), 10720, returning);
    gdImageDestroy(current_image);
    av_packet_free(&packet);
    delete[] buffer;
    fclose(returning);
    win_size.fetch_sub(1);
}
int proccesspagecaller(void* args) {

    pageproccessorparams* params = (pageproccessorparams*)args;
    
    proccesspage((params->packet), params->page, params->returningfiledir, params->pos, *(params->win_size));

    delete params->inthread;
    delete params;

    return 0;
}
int writefromrampagesthreadedcaller(void* args) {

    writebackparams* params = (writebackparams*)args;
    writefromrampagesthreaded(params);
    return 0;
}
int pushpagestoramcaller(void* args) {
    
    writebackparams* params = (writebackparams*)args;
    pushpagestoram(params->invideodir, params->outfiledir, params->pnglist_p, params->shared_value, params->has_ended);
    return 0;
}
bytes_6 decode_blocks_by_offset(gdImagePtr block, int offset_x, int offset_y, size_t page) {

    bytes_6 decoded;
    memset(&decoded, 0, sizeof(bytes_6));

    for (int i = 0; i < 6; i++) {
        unsigned char byte = 0;
        unsigned char mask = 0x80; // 10000000

        for (int j = 0; j < 8; j++) {
            const int base_x = offset_x + PIXELS[j][i][0];
            const int base_y = offset_y + PIXELS[j][i][1];
            int white_count = 0;

            // Unrolled pixel checks
            for (int dx = 0; dx < 4; dx++) {
                const int x = base_x + dx;

                white_count += gdImagePalettePixel(block, x, base_y);
                white_count += gdImagePalettePixel(block, x, base_y + 1);
                white_count += gdImagePalettePixel(block, x, base_y + 2);
                white_count += gdImagePalettePixel(block, x, base_y + 3);

            }

            if (white_count >= 8) {

                byte |= mask;
                if (white_count != 16)
                    printf("The pixel batch (%d,%d) at image %zu has %d white pixels instead of 16\n", base_x, base_y, page, white_count);

            }
            mask >>= 1;

        }

        decoded.bytes[i] = byte;
    }

    return decoded;
}
size_t freadbch(void* buffer, size_t element_size, size_t element_count, FILE* stream) {

    size_t total_element_count = element_size * element_count;
    _fseeki64(stream, -1 * total_element_count, SEEK_CUR);
    size_t return_val = fread(buffer, element_size, element_count, stream);
    _fseeki64(stream, -1 * total_element_count, SEEK_CUR);
    return return_val;
}
int64_t probevideo(char* invideodir) {

    // Initialize all pointers to NULL
    AVFormatContext* fmt_ctx = NULL;
    const AVCodec* dec_codec = NULL;
    int video_stream_idx = -1;
    int ret = 0;
    int64_t durationinsec = 0;
    int64_t durationinsecmod = 0;
    int64_t coefficentforonesecond = 0;
    int64_t total_frames = 0;

    // Open input file
    ret = avformat_open_input(&fmt_ctx, invideodir, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open input file\n");
        goto final_cleanup;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream information\n");
        goto final_cleanup;
    }

    durationinsec = (fmt_ctx->duration) / AV_TIME_BASE;
    durationinsecmod = fmt_ctx->duration % AV_TIME_BASE;
    if (durationinsecmod) {
        coefficentforonesecond = AV_TIME_BASE / durationinsecmod;
        total_frames = 60 * durationinsec + (60 / coefficentforonesecond);
    }
    else {
        total_frames = 60 * durationinsec;
    }

    total_frames -= total_frames % 60;
    total_frames++;

final_cleanup:
    avformat_close_input(&fmt_ctx);
    return total_frames;
}

void pipelinedramwriteback(void* args) {

    writebackparams* params = (writebackparams*)args;
    int64_t size = probevideo(params->invideodir);

    params->pnglist_p = new AVPacketPtr[size];

    thrd_t thrd1, thrd2;
    thrd_create(&thrd1, pushpagestoramcaller, args);
    thrd_create(&thrd2, writefromrampagesthreadedcaller, args);
    thrd_join(thrd1, NULL);
    printf("first thread joined\n");
    thrd_join(thrd2, NULL);
    printf("second thread joined\n");

    delete[] params->pnglist_p;
}




