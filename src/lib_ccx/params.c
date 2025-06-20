#include <png.h>
#include "protobuf-c.h"
#include "zlib.h"
#include "gpac/setup.h"
#include "gpac/version.h"
#include "lib_ccx.h"
#include "ccx_common_option.h"
#include "utility.h"
#include "activity.h"
#include "ccx_encoders_helpers.h"
#include "ccx_common_common.h"
#include "ccx_decoders_708.h"
#include "compile_info.h"
#include "../lib_hash/sha2.h"
#include <string.h>
#include <stdio.h>
#include <utf8proc/utf8proc.h>

#ifdef ENABLE_OCR
#include <tesseract/capi.h>
#include <leptonica/allheaders.h>
#endif

#ifdef ENABLE_HARDSUBX
#include "hardsubx.h"
#endif

#ifdef _WIN32
#define DEFAULT_FONT_PATH "C:\\Windows\\Fonts\\calibri.ttf"
#define DEFAULT_FONT_PATH_ITALICS "C:\\Windows\\Fonts\\calibrii.ttf"
#elif __APPLE__ // MacOS
#define DEFAULT_FONT_PATH "/System/Library/Fonts/Helvetica.ttc"
#define DEFAULT_FONT_PATH_ITALICS "/System/Library/Fonts/Helvetica-Oblique.ttf"
#else // Assume Linux
#define DEFAULT_FONT_PATH "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"
#define DEFAULT_FONT_PATH_ITALICS "/usr/share/fonts/truetype/noto/NotoSans-Italic.ttf"
#endif

#ifndef DISABLE_RUST
extern void ccxr_init_logger();
#endif

static int inputfile_capacity = 0;

size_t remove_trailing_whitespace(char *line)
{
	char *c = line + strlen(line) - 1;
	while (c >= line && isspace(*c))
		c--;
	c[1] = '\0';
	return c - line + 1;
}

int process_word_file(const char *filename, struct word_list *list)
{
	int ret = 0;
	FILE *fi;
	if ((fi = fopen(filename, "r")) == NULL)
	{
		mprint("\rUnable to open word list file: %s\n", filename);
		return -1;
	}

	char line[CCX_DECODER_608_SCREEN_WIDTH + 3]; // For screen width + (CR)LF + '\0' == screewdith + 3
	int num = 0;
	while (fgets(line, CCX_DECODER_608_SCREEN_WIDTH + 3, fi))
	{
		num++;
		if (line[0] == '#') // Treat lines starting with '#' as comments
			continue;

		size_t new_len = remove_trailing_whitespace(line);

		if (new_len > CCX_DECODER_608_SCREEN_WIDTH)
		{
			mprint("Word in line %d too long, max = %d characters.\n", num, CCX_DECODER_608_SCREEN_WIDTH);
			continue;
		}

		if (new_len > 0)
		{
			if (add_word(list, line) == -1)
			{
				ret = -1;
				break;
			}
		}
	}
	fclose(fi);
	return ret;
}

int isanumber(char *s)
{
	while (*s)
	{
		if (!isdigit(*s))
			return 0;
		s++;
	}
	return 1;
}

int parsedelay(struct ccx_s_options *opt, char *par)
{
	int sign = 0;
	char *c = par;
	while (*c)
	{
		if (*c == '-' || *c == '+')
		{
			if (c != par) // Sign only at the beginning
				return 1;
			if (*c == '-')
				sign = 1;
		}
		else
		{
			if (!isdigit(*c))
				return 1;
			opt->subs_delay = opt->subs_delay * 10 + (*c - '0');
		}
		c++;
	}
	if (sign)
		opt->subs_delay = -opt->subs_delay;
	return 0;
}

void set_binary_mode()
{
#ifdef WIN32
	setmode(fileno(stdin), O_BINARY);
#endif
}

int append_file_to_queue(struct ccx_s_options *opt, char *filename)
{
	if (filename[0] == '\0') // skip files with empty file name (ex : ./ccextractor "")
		return 0;
	char *c = (char *)malloc(strlen(filename) + 1);
	if (c == NULL)
		return -1;
	strcpy(c, filename);
	if (inputfile_capacity <= opt->num_input_files)
	{
		inputfile_capacity += 10;
		opt->inputfile = (char **)realloc(opt->inputfile, sizeof(char *) * inputfile_capacity);
		if (opt->inputfile == NULL)
		{
			free(c);
			return -1;
		}
	}
	opt->inputfile[opt->num_input_files] = c;
	opt->num_input_files++;
	return 0;
}

int add_file_sequence(struct ccx_s_options *opt, char *filename)
{
	int m, n;
	n = strlen(filename) - 1;
	// Look for the last digit in filename
	while (n >= 0 && !isdigit(filename[n]))
		n--;
	if (n == -1) // None. No expansion needed
		return append_file_to_queue(opt, filename);
	m = n;
	while (m >= 0 && isdigit(filename[m]))
		m--;
	m++;
	// Here: Significant digits go from filename[m] to filename[n]
	char *num = (char *)malloc(n - m + 2);
	if (!num)
		return -1;
	strncpy(num, filename + m, n - m + 1);
	num[n - m + 1] = 0;
	int i = atoi(num);
	char *temp = (char *)malloc(n - m + 3); // For overflows
	if (!temp)
	{
		free(num);
		return -1;
	}
	// printf ("Expanding %d to %d, initial value=%d\n",m,n,i);
	for (;;)
	{
		FILE *f = fopen(filename, "r");
		if (f == NULL) // Doesn't exist or we can't read it. We're done
			break;
		fclose(f);
		if (append_file_to_queue(opt, filename)) // Memory panic
		{
			free(num);
			free(temp);
			return -1;
		}
		i++;
		sprintf(temp, "%d", i);

		int temp_len = strlen(temp);
		int num_len = strlen(num);
		if (temp_len > num_len) // From 999 to 1000, etc.
			break;
		memcpy(filename + m + (num_len - temp_len), temp, temp_len);
		memset(filename + m, '0', num_len - temp_len);
	}
	free(num);
	free(temp);
	return 0;
}

void set_output_format(struct ccx_s_options *opt, const char *format)
{
	opt->write_format_rewritten = 1;

	while (*format == '-')
		format++;

	if (opt->send_to_srv && strcmp(format, "bin") != 0)
	{
		mprint("Output format is changed to bin\n");
		format = "bin";
	}

	if (strcmp(format, "ass") == 0)
	{
		opt->write_format = CCX_OF_SSA;
		opt->use_ass_instead_of_ssa = 1;
	}
	else if (strcmp(format, "ccd") == 0)
		opt->write_format = CCX_OF_CCD;
	else if (strcmp(format, "scc") == 0)
		opt->write_format = CCX_OF_SCC;
	else if (strcmp(format, "srt") == 0)
		opt->write_format = CCX_OF_SRT;
	else if (strcmp(format, "ssa") == 0)
		opt->write_format = CCX_OF_SSA;
	else if (strcmp(format, "webvtt") == 0)
		opt->write_format = CCX_OF_WEBVTT;
	else if (strcmp(format, "webvtt-full") == 0)
	{
		opt->write_format = CCX_OF_WEBVTT;
		opt->use_webvtt_styling = 1;
	}
	else if (strcmp(format, "sami") == 0 || strcmp(format, "smi") == 0)
		opt->write_format = CCX_OF_SAMI;
	else if (strcmp(format, "transcript") == 0 || strcmp(format, "txt") == 0)
	{
		opt->write_format = CCX_OF_TRANSCRIPT;
		opt->settings_dtvcc.no_rollup = 1;
	}
	else if (strcmp(format, "timedtranscript") == 0 || strcmp(format, "ttxt") == 0)
	{
		opt->write_format = CCX_OF_TRANSCRIPT;
		if (opt->date_format == ODF_NONE)
			opt->date_format = ODF_HHMMSSMS;
		// Sets the right things so that timestamps and the mode are printed.
		if (!opt->transcript_settings.isFinal)
		{
			opt->transcript_settings.showStartTime = 1;
			opt->transcript_settings.showEndTime = 1;
			opt->transcript_settings.showCC = 0;
			opt->transcript_settings.showMode = 1;
		}
	}
	else if (strcmp(format, "report") == 0)
	{
		opt->write_format = CCX_OF_NULL;
		opt->messages_target = 0;
		opt->print_file_reports = 1;
		opt->demux_cfg.ts_allprogram = CCX_TRUE;
	}
	else if (strcmp(format, "raw") == 0)
		opt->write_format = CCX_OF_RAW;
	else if (strcmp(format, "smptett") == 0)
		opt->write_format = CCX_OF_SMPTETT;
	else if (strcmp(format, "bin") == 0)
		opt->write_format = CCX_OF_RCWT;
	else if (strcmp(format, "null") == 0)
		opt->write_format = CCX_OF_NULL;
	else if (strcmp(format, "dvdraw") == 0)
		opt->write_format = CCX_OF_DVDRAW;
	else if (strcmp(format, "spupng") == 0)
		opt->write_format = CCX_OF_SPUPNG;
	else if (strcmp(format, "simplexml") == 0)
		opt->write_format = CCX_OF_SIMPLE_XML;
	else if (strcmp(format, "g608") == 0)
		opt->write_format = CCX_OF_G608;
#ifdef WITH_LIBCURL
	else if (strcmp(format, "curl") == 0)
		opt->write_format = CCX_OF_CURL;
#endif
	else if (strcmp(format, "mcc") == 0)
		opt->write_format = CCX_OF_MCC;
	else
		fatal(EXIT_MALFORMED_PARAMETER, "Unknown output file format: %s\n", format);
}

void set_input_format(struct ccx_s_options *opt, const char *format)
{
	if (opt->input_source == CCX_DS_TCP && strcmp(format, "bin") != 0)
	{
		mprint("Input format is changed to bin\n");
		format = "bin";
	}

	while (*format == '-')
		format++;
	if (strcmp(format, "es") == 0) // Does this actually do anything?
		opt->demux_cfg.auto_stream = CCX_SM_ELEMENTARY_OR_NOT_FOUND;
	else if (strcmp(format, "ts") == 0)
	{
		opt->demux_cfg.auto_stream = CCX_SM_TRANSPORT;
		opt->demux_cfg.m2ts = 0;
	}
	else if (strcmp(format, "m2ts") == 0)
	{
		opt->demux_cfg.auto_stream = CCX_SM_TRANSPORT;
		opt->demux_cfg.m2ts = 1;
	}
	else if (strcmp(format, "ps") == 0 || strcmp(format, "nots") == 0)
		opt->demux_cfg.auto_stream = CCX_SM_PROGRAM;
	else if (strcmp(format, "asf") == 0 || strcmp(format, "dvr-ms") == 0)
		opt->demux_cfg.auto_stream = CCX_SM_ASF;
	else if (strcmp(format, "wtv") == 0)
		opt->demux_cfg.auto_stream = CCX_SM_WTV;
	else if (strcmp(format, "raw") == 0)
		opt->demux_cfg.auto_stream = CCX_SM_MCPOODLESRAW;
	else if (strcmp(format, "bin") == 0)
		opt->demux_cfg.auto_stream = CCX_SM_RCWT;
	else if (strcmp(format, "mp4") == 0)
		opt->demux_cfg.auto_stream = CCX_SM_MP4;
	else if (strcmp(format, "mkv") == 0)
		opt->demux_cfg.auto_stream = CCX_SM_MKV;
	else if (strcmp(format, "mxf") == 0)
		opt->demux_cfg.auto_stream = CCX_SM_MXF;
#ifdef WTV_DEBUG
	else if (strcmp(format, "hex") == 0)
		opt->demux_cfg.auto_stream = CCX_SM_HEX_DUMP;
#endif
	else
		fatal(EXIT_MALFORMED_PARAMETER, "Unknown input file format: %s\n", format);
}

void print_usage(void)
{
	mprint("Originally based on McPoodle's tools. Check his page for lots of information\n");
	mprint("on closed captions technical details.\n");
	mprint("(http://www.theneitherworld.com/mcpoodle/SCC_TOOLS/DOCS/SCC_TOOLS.HTML)\n\n");
	mprint("This tool home page:\n");
	mprint("http://www.ccextractor.org\n");
	mprint("  Extracts closed captions and teletext subtitles from video streams.\n");
	mprint("    (DVB, .TS, ReplayTV 4000 and 5000, dvr-ms, bttv, Tivo, Dish Network,\n");
	mprint("     .mp4, HDHomeRun are known to work).\n\n");
	mprint("  Syntax:\n");
	mprint("  ccextractor [options] inputfile1 [inputfile2...] [-o outputfilename]\n");
	mprint("\n");
	mprint("To see This Help Message: -h or --help\n\n");
	mprint("File name related options:\n");
	mprint("            inputfile: file(s) to process\n");
	mprint("    -o outputfilename: Use -o parameters to define output filename if you don't\n");
	mprint("                       like the default ones (same as infile plus _1 or _2 when\n");
	mprint("                       needed and file extension, e.g. .srt).\n");
	mprint("              --stdout: Write output to stdout (console) instead of file. If\n");
	mprint("                       stdout is used, then -o can't be used. Also\n");
	mprint("                       --stdout will redirect all messages to stderr (error).\n");
	mprint("           --pesheader: Dump the PES Header to stdout (console). This is\n");
	mprint("                       used for debugging purposes to see the contents\n");
	mprint("                       of each PES packet header.\n");
	mprint("         --debugdvbsub: Write the DVB subtitle debug traces to console.\n");
	mprint("      --ignoreptsjumps: Ignore PTS jumps (default).\n");
	mprint("         --fixptsjumps: fix pts jumps. Use this parameter if you\n");
	mprint("                       experience timeline resets/jumps in the output.\n");
	mprint("               --stdin: Reads input from stdin (console) instead of file.\n");
	mprint("                       Alternatively, - can be used instead of -stdin\n");
	mprint("Output file segmentation:\n");
	mprint("    --outinterval x output in interval of x seconds\n");
	mprint("   --segmentonkeyonly: When segmenting files, do it only after a I frame\n");
	mprint("                            trying to behave like FFmpeg\n\n");
	mprint("Network support:\n");
	mprint("      --udp [[src@]host:]port: Read the input via UDP (listening in the specified\n");
	mprint("                              port) instead of reading a file. Host and src can be a\n");
	mprint("                              hostname or IPv4 address. If host is not specified\n");
	mprint("                              then listens on the local host.\n\n");
	mprint("            --sendto host[:port]: Sends data in BIN format to the server\n");
	mprint("                                 according to the CCExtractor's protocol over\n");
	mprint("                                 TCP. For IPv6 use [address]:port\n");
	mprint("            --tcp port: Reads the input data in BIN format according to\n");
	mprint("                        CCExtractor's protocol, listening specified port on the\n");
	mprint("                        local host\n");
	mprint("            --tcp-password password: Sets server password for new connections to\n");
	mprint("                                   tcp server\n");
	mprint("            --tcp-description description: Sends to the server short description about\n");
	mprint("                                  captions e.g. channel name or file name\n");
	mprint("Options that affect what will be processed:\n");
	mprint("      --output-field 1 / 2 / both: Output Field 1 data, Field 2 data, or both\n");
	mprint("                       (DEFAULT is 1)\n");
	mprint("Use --append to prevent overwriting of existing files. The output will be\n");
	mprint("      appended instead.\n");
	mprint("                 --cc2: When in srt/sami mode, process captions in channel 2\n");
	mprint("                       instead of channel 1. Alternatively, --CC2 can also be used.\n");
	mprint("	--service N1[cs1],N2[cs2]...:\n");
	mprint("                       Enable CEA-708 (DTVCC) captions processing for the listed\n");
	mprint("                       services. The parameter is a comma delimited list\n");
	mprint("                       of services numbers, such as \"1,2\" to process the\n");
	mprint("                       primary and secondary language services.\n");
	mprint("                       Pass \"all\" to process all services found.\n");
	mprint("\n");
	mprint("                       If captions in a service are stored in 16-bit encoding,\n");
	mprint("                       you can specify what charset or encoding was used. Pass\n");
	mprint("                       its name after service number (e.g. \"1[EUC-KR],3\" or\n");
	mprint("                       \"all[EUC-KR]\") and it will encode specified charset to\n");
	mprint("                       UTF-8 using iconv. See iconv documentation to check if\n");
	mprint("                       required encoding/charset is supported.\n");
	mprint("\n");
	mprint("Input formats:\n");
	mprint("       With the exception of McPoodle's raw format, which is just the closed\n");
	mprint("       caption data with no other info, CCExtractor can usually detect the\n");
	mprint("       input format correctly. To force a specific format:\n\n");
	mprint("                  -in=format\n\n");
	mprint("       where format is one of these:\n");
	mprint("                       ts   -> For Transport Streams.\n");
	mprint("                       ps   -> For Program Streams.\n");
	mprint("                       es   -> For Elementary Streams.\n");
	mprint("                       asf  -> ASF container (such as DVR-MS).\n");
	mprint("                       wtv  -> Windows Television (WTV)\n");
	mprint("                       bin  -> CCExtractor's own binary format.\n");
	mprint("                       raw  -> For McPoodle's raw files.\n");
	mprint("                       mp4  -> MP4/MOV/M4V and similar.\n");
	mprint("                       m2ts -> BDAV MPEG-2 Transport Stream\n");
	mprint("                       mkv  -> Matroska container and WebM.\n");
	mprint("                       mxf  -> Material Exchange Format (MXF).\n");
#ifdef WTV_DEBUG
	mprint("                       hex  -> Hexadecimal dump as generated by wtvccdump.\n");
#endif
	mprint("       --ts, --ps, --es, --mp4, --wtv, --mkv and --asf/--dvr-ms can be used as shorts.\n\n");
	mprint("Output formats:\n\n");
	mprint("                 --out=format\n\n");
	mprint("       where format is one of these:\n");
	mprint("                      srt     -> SubRip (default, so not actually needed).\n");
	mprint("                      ass/ssa -> SubStation Alpha.\n");
	mprint("                      ccd     -> Scenarist Closed Caption Disassembly format\n");
	mprint("                      scc     -> Scenarist Closed Caption format\n");
	mprint("                      webvtt  -> WebVTT format\n");
	mprint("                      webvtt-full -> WebVTT format with styling\n");
	mprint("                      sami    -> MS Synchronized Accesible Media Interface.\n");
	mprint("                      bin     -> CC data in CCExtractor's own binary format.\n");
	mprint("                      raw     -> CC data in McPoodle's Broadcast format.\n");
	mprint("                      dvdraw  -> CC data in McPoodle's DVD format.\n");
	mprint("                      mcc     -> CC data compressed using MacCaption Format.\n");
	mprint("                      txt     -> Transcript (no time codes, no roll-up\n");
	mprint("                                 captions, just the plain transcription.\n");
	mprint("                      ttxt    -> Timed Transcript (transcription with time\n");
	mprint("                                 info)\n");
	mprint("                      g608    -> Grid 608 format.\n");
#ifdef WITH_LIBCURL
	mprint("                      curl    -> POST plain transcription frame-by-frame to a\n");
	mprint("                                 URL specified by -curlposturl. Don't produce\n");
	mprint("                                 any file output.\n");
#endif
	mprint("                      smptett -> SMPTE Timed Text (W3C TTML) format.\n");
	mprint("                      spupng  -> Set of .xml and .png files for use with\n");
	mprint("                                 dvdauthor's spumux.\n");
	mprint("                                 See \"Notes on spupng output format\"\n");
	mprint("                      null    -> Don't produce any file output\n");
	mprint("                      report  -> Prints to stdout information about captions\n");
	mprint("                                 in specified input. Don't produce any file\n");
	mprint("                                 output\n\n");
	mprint("       --srt, --dvdraw, --sami, --webvtt, --txt, --ttxt and --null can be used as shorts.\n\n");

	mprint("Options that affect how input files will be processed.\n");

	mprint("       --goptime: Use GOP for timing instead of PTS. This only applies\n");
	mprint("                       to Program or Transport Streams with MPEG2 data and\n");
	mprint("                       overrides the default PTS timing.\n");
	mprint("                       GOP timing is always used for Elementary Streams.\n");
	mprint("	   --no-goptime: Never use GOP timing (use PTS), even if ccextractor\n");
	mprint("                       detects GOP timing is the reasonable choice.\n");
	mprint("       --fixpadding: Fix padding - some cards (or providers, or whatever)\n");
	mprint("                       seem to send 0000 as CC padding instead of 8080. If you\n");
	mprint("                       get bad timing, this might solve it.\n");
	mprint("               --90090: Use 90090 (instead of 90000) as MPEG clock frequency.\n");
	mprint("                       (reported to be needed at least by Panasonic DMR-ES15\n");
	mprint("                       DVD Recorder)\n");
	mprint("       --videoedited: By default, ccextractor will process input files in\n");
	mprint("                       sequence as if they were all one large file (i.e.\n");
	mprint("                       split by a generic, non video-aware tool. If you\n");
	mprint("                       are processing video hat was split with a editing\n");
	mprint("                       tool, use --ve so ccextractor doesn't try to rebuild\n");
	mprint("                       the original timing.\n");
	mprint("   -s --stream [secs]: Consider the file as a continuous stream that is\n");
	mprint("                       growing as ccextractor processes it, so don't try\n");
	mprint("                       to figure out its size and don't terminate processing\n");
	mprint("                       when reaching the current end (i.e. wait for more\n");
	mprint("                       data to arrive). If the optional parameter secs is\n");
	mprint("                       present, it means the number of seconds without any\n");
	mprint("                       new data after which ccextractor should exit. Use\n");
	mprint("                       this parameter if you want to process a live stream\n");
	mprint("                       but not kill ccextractor externally.\n");
	mprint("                       Note: If -s is used then only one input file is\n");
	mprint("                       allowed.\n");
	mprint("      --usepicorder: Use the pic_order_cnt_lsb in AVC/H.264 data streams\n");
	mprint("                       to order the CC information.  The default way is to\n");
	mprint("                       use the PTS information.  Use this switch only when\n");
	mprint("                       needed.\n");
	mprint("                --myth: Force MythTV code branch.\n");
	mprint("              --no-myth: Disable MythTV code branch.\n");
	mprint("                       The MythTV branch is needed for analog captures where\n");
	mprint("                       the closed caption data is stored in the VBI, such as\n");
	mprint("                       those with bttv cards (Hauppage 250 for example). This\n");
	mprint("                       is detected automatically so you don't need to worry\n");
	mprint("                       about this unless autodetection doesn't work for you.\n");
	mprint("       --wtvconvertfix: This switch works around a bug in Windows 7's built in\n");
	mprint("                       software to convert *.wtv to *.dvr-ms. For analog NTSC\n");
	mprint("                       recordings the CC information is marked as digital\n");
	mprint("                       captions. Use this switch only when needed.\n");
	mprint("            --wtvmpeg2: Read the captions from the MPEG2 video stream rather\n");
	mprint("                       than the captions stream in WTV files\n");
	mprint("     --program-number: In TS mode, specifically select a program to process.\n");
	mprint("                       Not needed if the TS only has one. If this parameter\n");
	mprint("                       is not specified and CCExtractor detects more than one\n");
	mprint("                       program in the input, it will list the programs found\n");
	mprint("                       and terminate without doing anything, unless\n");
	mprint("                       --autoprogram (see below) is used.\n");
	mprint("         --autoprogram: If there's more than one program in the stream, just use\n");
	mprint("                       the first one we find that contains a suitable stream.\n");
	mprint("        --multiprogram: Uses multiple programs from the same input stream.\n");
	mprint("             --datapid: Don't try to find out the stream for caption/teletext\n");
	mprint("                       data, just use this one instead.\n");
	mprint("      --datastreamtype: Instead of selecting the stream by its PID, select it\n");
	mprint("                       by its type (pick the stream that has this type in\n");
	mprint("                       the PMT)\n");
	mprint("          --streamtype: Assume the data is of this type, don't autodetect. This\n");
	mprint("                       parameter may be needed if --datapid or -datastreamtype\n");
	mprint("                       is used and CCExtractor cannot determine how to process\n");
	mprint("                       the stream. The value will usually be 2 (MPEG video) or\n");
	mprint("                       6 (MPEG private data).\n");
	mprint("    	  --hauppauge: If the video was recorder using a Hauppauge card, it\n");
	mprint("                       might need special processing. This parameter will\n");
	mprint("                       force the special treatment.\n");
	mprint("         --mp4vidtrack: In MP4 files the closed caption data can be embedded in\n");
	mprint("                       the video track or in a dedicated CC track. If a\n");
	mprint("                       dedicated track is detected it will be processed instead\n");
	mprint("                       of the video track. If you need to force the video track\n");
	mprint("                       to be processed instead use this option.\n");
	mprint("       --no-autotimeref: Some streams come with broadcast date information. When\n");
	mprint("                       such data is available, CCExtractor will set its time\n");
	mprint("                       reference to the received data. Use this parameter if\n");
	mprint("                       you prefer your own reference. Note: Current this only\n");
	mprint("                       affects Teletext in timed transcript with -datets.\n");
	mprint("           --no-scte20: Ignore SCTE-20 data if present.\n");
	mprint("  --webvtt-create-css: Create a separate file for CSS instead of inline.\n");
	mprint("              --deblev: Enable debug so the calculated distance for each two\n");
	mprint("                       strings is displayed. The output includes both strings,\n");
	mprint("                       the calculated distance, the maximum allowed distance,\n");
	mprint("                       and whether the strings are ultimately considered  \n");
	mprint("                       equivalent or not, i.e. the calculated distance is \n");
	mprint("                       less or equal than the max allowed..\n");
	mprint("	   --analyzevideo  Analyze the video stream even if it's not used for\n");
	mprint("                       subtitles. This allows to provide video information.\n");
	mprint("  --timestamp-map      Enable the X-TIMESTAMP-MAP header for WebVTT (HLS)\n");
	mprint("Levenshtein distance:\n");
	mprint("           --no-levdist: Don't attempt to correct typos with Levenshtein distance.\n");
	mprint(" --levdistmincnt value: Minimum distance we always allow regardless\n");
	mprint("                       of the length of the strings.Default 2. \n");
	mprint("                       This means that if the calculated distance \n");
	mprint("                       is 0,1 or 2, we consider the strings to be equivalent.\n");
	mprint(" --levdistmaxpct value: Maximum distance we allow, as a percentage of\n");
	mprint("                       the shortest string length. Default 10%.\n");
	mprint("                       For example, consider a comparison of one string of \n");
	mprint("	                    30 characters and one of 60 characters. We want to \n");
	mprint("                       determine whether the first 30 characters of the longer\n");
	mprint("                       string are more or less the same as the shortest string,\n");
	mprint("	                    i.e. whether the longest string  is the shortest one\n");
	mprint("                       plus new characters and maybe some corrections. Since\n");
	mprint("                       the shortest string is 30 characters and  the default\n");
	mprint("                       percentage is 10%, we would allow a distance of up \n");
	mprint("                       to 3 between the first 30 characters.\n");
	mprint("\n");
	mprint("Options that affect what kind of output will be produced:\n");
	mprint("            --chapters: (Experimental) Produces a chapter file from MP4 files.\n");
	mprint("                       Note that this must only be used with MP4 files,\n");
	mprint("                       for other files it will simply generate subtitles file.\n");
	mprint("                 --bom: Append a BOM (Byte Order Mark) to output files.\n");
	mprint("                       Note that most text processing tools in linux will not\n");
	mprint("                       like BOM.\n");
	mprint("                       --no-bom: Do not append a BOM (Byte Order Mark) to output\n");
	mprint("                       files. Note that this may break files when using\n");
	mprint("                       Windows. This is the default in non-Windows builds.\n");
	mprint("             --unicode: Encode subtitles in Unicode instead of Latin-1.\n");
	mprint("                --utf8: Encode subtitles in UTF-8 (no longer needed.\n");
	mprint("                       because UTF-8 is now the default).\n");
	mprint("              --latin1: Encode subtitles in Latin-1\n");
	mprint("        --no-fontcolor: For .srt/.sami/.vtt, don't add font color tags.\n");
	mprint("       --no-htmlescape: For .srt/.sami/.vtt, don't covert html unsafe character\n");
	mprint("      --no-typesetting: For .srt/.sami/.vtt, don't add typesetting tags.\n");
	mprint("                --trim: Trim lines.\n");
	mprint("        --defaultcolor: Select a different default color (instead of\n");
	mprint("                       white). This causes all output in .srt/.smi/.vtt\n");
	mprint("                       files to have a font tag, which makes the files\n");
	mprint("                       larger. Add the color you want in RGB, such as\n");
	mprint("                       --dc #FF0000 for red.\n");
	mprint("         --sentencecap: Sentence capitalization. Use if you hate\n");
	mprint("                       ALL CAPS in subtitles.\n");
	mprint("        --capfile file: Add the contents of 'file' to the list of words\n");
	mprint("                       that must be capitalized. For example, if file\n");
	mprint("                       is a plain text file that contains\n\n");
	mprint("                       Tony\n");
	mprint("                       Alan\n\n");
	mprint("                       Whenever those words are found they will be written\n");
	mprint("                       exactly as they appear in the file.\n");
	mprint("                       Use one line per word. Lines starting with # are\n");
	mprint("                       considered comments and discarded.\n\n");
	mprint("                 --kf: Censors profane words from subtitles.\n");
	mprint("--profanity-file <file>: Add the contents of <file> to the list of words that.\n");
	mprint("                         must be censored. The content of <file>, follows the\n");
	mprint("                         same syntax as for the capitalization file\n");
	mprint("      --splitbysentence: Split output text so each frame contains a complete\n");
	mprint("                       sentence. Timings are adjusted based on number of\n");
	mprint("                       characters\n");
	mprint("          --unixts REF: For timed transcripts that have an absolute date\n");
	mprint("                       instead of a timestamp relative to the file start), use\n");
	mprint("                       this time reference (UNIX timestamp). 0 => Use current\n");
	mprint("                       system time.\n");
	mprint("                       ccextractor will automatically switch to transport\n");
	mprint("                       stream UTC timestamps when available.\n");
	mprint("              --datets: In transcripts, write time as YYYYMMDDHHMMss,ms.\n");
	mprint("               --sects: In transcripts, write time as ss,ms\n");
	mprint("                --UCLA: Transcripts are generated with a specific format\n");
	mprint("                       that is convenient for a specific project, feel\n");
	mprint("                       free to play with it but be aware that this format\n");
	mprint("                       is really live - don't rely on its output format\n");
	mprint("                       not changing between versions.\n");
	mprint("            --latrusmap Map Latin symbols to Cyrillic ones in special cases\n");
	mprint("                       of Russian Teletext files (issue #1086)\n");
	mprint("                 --xds: In timed transcripts, all XDS information will be saved\n");
	mprint("                       to the output file.\n");
	mprint("                  --lf: Use LF (UNIX) instead of CRLF (DOS, Windows) as line\n");
	mprint("                       terminator.\n");
	mprint("                  --df: For MCC Files, force dropframe frame count.\n");
	mprint("            --autodash: Based on position on screen, attempt to determine\n");
	mprint("                       the different speakers and a dash (-) when each\n");
	mprint("                       of them talks (.srt/.vtt only, --trim required).\n");
	mprint("          --xmltv mode: produce an XMLTV file containing the EPG data from\n");
	mprint("                       the source TS file. Mode: 1 = full output\n");
	mprint("                       2 = live output. 3 = both\n");
	mprint(" --xmltvliveinterval x: interval of x seconds between writing live mode xmltv output.\n");
	mprint("--xmltvoutputinterval x: interval of x seconds between writing full file xmltv output.\n");
	mprint("    --xmltvonlycurrent: Only print current events for xmltv output.\n");
	mprint("                 --sem: Create a .sem file for each output file that is open\n");
	mprint("                       and delete it on file close.\n");
	mprint("             --dvblang: For DVB subtitles, select which language's caption\n");
	mprint("                       stream will be processed. e.g. 'eng' for English.\n");
	mprint("                       If there are multiple languages, only this specified\n");
	mprint("                       language stream will be processed (default).\n");
	mprint("             --ocrlang: Manually select the name of the Tesseract .traineddata\n");
	mprint("                       file. Helpful if you want to OCR a caption stream of\n");
	mprint("                       one language with the data of another language.\n");
	mprint("                       e.g. '-dvblang chs --ocrlang chi_tra' will decode the\n");
	mprint("                       Chinese (Simplified) caption stream but perform OCR\n");
	mprint("                       using the Chinese (Traditional) trained data\n");
	mprint("                       This option is also helpful when the traineddata file\n");
	mprint("                       has non standard names that don't follow ISO specs\n");
	mprint("          --quant mode: How to quantize the bitmap before passing it to tesseract\n");
	mprint("                       for OCR'ing.\n");
	mprint("                       0: Don't quantize at all.\n");
	mprint("                       1: Use CCExtractor's internal function (default).\n");
	mprint("                       2: Reduce distinct color count in image for faster results.\n");
	mprint("                 --oem: Select the OEM mode for Tesseract.\n");
	mprint("                       Available modes :\n");
	mprint("                       0: OEM_TESSERACT_ONLY - the fastest mode.\n");
	mprint("                       1: OEM_LSTM_ONLY - use LSTM algorithm for recognition.\n");
	mprint("                       2: OEM_TESSERACT_LSTM_COMBINED - both algorithms.\n");
	mprint("                       Default value depends on the tesseract version linked :\n");
	mprint("                       Tesseract v3 : default mode is 0,\n");
	mprint("                       Tesseract v4 : default mode is 1.\n");
	mprint("             --mkvlang: For MKV subtitles, select which language's caption\n");
	mprint("                       stream will be processed. e.g. 'eng' for English.\n");
	mprint("                       Language codes can be either the 3 letters bibliographic\n");
	mprint("                       ISO-639-2 form (like \"fre\" for french) or a language\n");
	mprint("                       code followed by a dash and a country code for specialities\n");
	mprint("                       in languages (like \"fre-ca\" for Canadian French).\n");
	mprint("          --no-spupngocr When processing DVB don't use the OCR to write the text as\n");
	mprint("                       comments in the XML file.\n");
	mprint("                --font: Specify the full path of the font that is to be used when\n");
	mprint("                       generating SPUPNG files. If not specified, you need to\n");
	mprint("                       have the default font installed (Helvetica for macOS, Calibri\n");
	mprint("                       for Windows, and Noto for other operating systems at their)\n");
	mprint("                       default location)\n");
	mprint("             --italics: Specify the full path of the italics font that is to be used when\n");
	mprint("                       generating SPUPNG files. If not specified, you need to\n");
	mprint("                       have the default font installed (Helvetica Oblique for macOS, Calibri Italic\n");
	mprint("                       for Windows, and NotoSans Italic for other operating systems at their)\n");
	mprint("                       default location)\n");
	mprint("\n");
	mprint("Options that affect how ccextractor reads and writes (buffering):\n");

	mprint("         --bufferinput: Forces input buffering.\n");
	mprint("       --no-bufferinput: Disables input buffering.\n");
	mprint("      --buffersize val: Specify a size for reading, in bytes (suffix with K or\n");
	mprint("                       or M for kilobytes and megabytes). Default is 16M.\n");
	mprint("                 --koc: keep-output-close. If used then CCExtractor will close\n");
	mprint("                       the output file after writing each subtitle frame and\n");
	mprint("                       attempt to create it again when needed.\n");
	mprint("          --forceflush: Flush the file buffer whenever content is written.\n");
	mprint("\n");

	mprint("Options that affect the built-in 608 closed caption decoder:\n");

	mprint("                 --dru: Direct Roll-Up. When in roll-up mode, write character by\n");
	mprint("                       character instead of line by line. Note that this\n");
	mprint("                       produces (much) larger files.\n");
	mprint("           --no-rollup: If you hate the repeated lines caused by the roll-up\n");
	mprint("                       emulation, you can have ccextractor write only one\n");
	mprint("                       line at a time, getting rid of these repeated lines.\n");
	mprint("     --ru1 / ru2 / ru3: roll-up captions can consist of 2, 3 or 4 visible\n");
	mprint("                       lines at any time (the number of lines is part of\n");
	mprint("                       the transmission). If having 3 or 4 lines annoys\n");
	mprint("                       you you can use --ru to force the decoder to always\n");
	mprint("                       use 1, 2 or 3 lines. Note that 1 line is not\n");
	mprint("                       a real mode rollup mode, so CCExtractor does what\n");
	mprint("                       it can.\n");
	mprint("                       In --ru1 the start timestamp is actually the timestamp\n");
	mprint("                       of the first character received which is possibly more\n");
	mprint("                       accurate.\n");
	mprint("\n");

	mprint("Options that affect timing:\n");

	mprint("            --delay ms: For srt/sami/webvtt, add this number of milliseconds to\n");
	mprint("                       all times. For example, --delay 400 makes subtitles\n");
	mprint("                       appear 400ms late. You can also use negative numbers\n");
	mprint("                       to make subs appear early.\n");

	mprint("Options that affect what segment of the input file(s) to process:\n");

	mprint("        --startat time: Only write caption information that starts after the\n");
	mprint("                       given time.\n");
	mprint("                       Time can be seconds, MM:SS or HH:MM:SS.\n");
	mprint("                       For example, --startat 3:00 means 'start writing from\n");
	mprint("                       minute 3.\n");
	mprint("          --endat time: Stop processing after the given time (same format as\n");
	mprint("                       -startat).\n");
	mprint("                       The --startat and --endat options are honored in all\n");
	mprint("                       output formats.  In all formats with timing information\n");
	mprint("                       the times are unchanged.\n");
	mprint("      --screenfuls num: Write 'num' screenfuls and terminate processing.\n\n");

	mprint("Options that affect which codec is to be used have to be searched in input\n");

	mprint("      --codec dvbsub    select the dvb subtitle from all elementary stream,\n"
	       "                        if stream of dvb subtitle type is not found then \n"
	       "                        nothing is selected and no subtitle is generated\n"
	       "      --no-codec dvbsub   ignore dvb subtitle and follow default behaviour\n"
	       "      --codec teletext   select the teletext subtitle from elementary stream\n"
	       "      --no-codec teletext ignore teletext subtitle\n");

	mprint("Adding start and end credits:\n");

	mprint("        --startcreditstext txt: Write this text as start credits. If there are\n");
	mprint("                                several lines, separate them with the\n");
	mprint("                                characters \\n, for example Line1\\nLine 2.\n");
	mprint("  --startcreditsnotbefore time: Don't display the start credits before this\n");
	mprint("                                time (S, or MM:SS). Default: %s\n", DEF_VAL_STARTCREDITSNOTBEFORE);
	mprint("   --startcreditsnotafter time: Don't display the start credits after this\n");
	mprint("                                time (S, or MM:SS). Default: %s\n", DEF_VAL_STARTCREDITSNOTAFTER);
	mprint(" --startcreditsforatleast time: Start credits need to be displayed for at least\n");
	mprint("                                this time (S, or MM:SS). Default: %s\n", DEF_VAL_STARTCREDITSFORATLEAST);
	mprint("  --startcreditsforatmost time: Start credits should be displayed for at most\n");
	mprint("                                this time (S, or MM:SS). Default: %s\n", DEF_VAL_STARTCREDITSFORATMOST);
	mprint("          --endcreditstext txt: Write this text as end credits. If there are\n");
	mprint("                                several lines, separate them with the\n");
	mprint("                                characters \\n, for example Line1\\nLine 2.\n");
	mprint("   --endcreditsforatleast time: End credits need to be displayed for at least\n");
	mprint("                                this time (S, or MM:SS). Default: %s\n", DEF_VAL_ENDCREDITSFORATLEAST);
	mprint("    --endcreditsforatmost time: End credits should be displayed for at most\n");
	mprint("                                this time (S, or MM:SS). Default: %s\n", DEF_VAL_ENDCREDITSFORATMOST);
	mprint("\n");

	mprint("Options that affect debug data:\n");

	mprint("               --debug: Show lots of debugging output.\n");
	mprint("                 --608: Print debug traces from the EIA-608 decoder.\n");
	mprint("                       If you need to submit a bug report, please send\n");
	mprint("                       the output from this option.\n");
	mprint("                 --708: Print debug information from the (currently\n");
	mprint("                       in development) EIA-708 (DTV) decoder.\n");
	mprint("              --goppts: Enable lots of time stamp output.\n");
	mprint("            --xdsdebug: Enable XDS debug data (lots of it).\n");
	mprint("               --vides: Print debug info about the analysed elementary\n");
	mprint("                       video stream.\n");
	mprint("               --cbraw: Print debug trace with the raw 608/708 data with\n");
	mprint("                       time stamps.\n");
	mprint("              --no-sync: Disable the syncing code.  Only useful for debugging\n");
	mprint("                       purposes.\n");
	mprint("             --fullbin: Disable the removal of trailing padding blocks\n");
	mprint("                       when exporting to bin format.  Only useful for\n");
	mprint("                       for debugging purposes.\n");
	mprint("          --parsedebug: Print debug info about the parsed container\n");
	mprint("                       file. (Only for TS/ASF files at the moment.)\n");
	mprint("            --parsePAT: Print Program Association Table dump.\n");
	mprint("            --parsePMT: Print Program Map Table dump.\n");
	mprint("              --dumpdef: Hex-dump defective TS packets.\n");
	mprint(" --investigate-packets: If no CC packets are detected based on the PMT, try\n");
	mprint("                       to find data in all packets by scanning.\n");
#ifdef ENABLE_SHARING
	mprint("       -sharing-debug: Print extracted CC sharing service messages\n");
#endif // ENABLE_SHARING
	mprint("\n");

	mprint("Teletext related options:\n");

	mprint("          --tpage page: Use this page for subtitles (if this parameter\n");
	mprint("                       is not used, try to autodetect). In Spain the\n");
	mprint("                       page is always 888, may vary in other countries.\n");
	mprint("            --tverbose: Enable verbose mode in the teletext decoder.\n\n");
	mprint("            --teletext: Force teletext mode even if teletext is not detected.\n");
	mprint("                       If used, you should also pass --datapid to specify\n");
	mprint("                       the stream ID you want to process.\n");
	mprint("          --no-teletext: Disable teletext processing. This might be needed\n");
	mprint("                       for video streams that have both teletext packets\n");
	mprint("                       and CEA-608/708 packets (if teletext is processed\n");
	mprint("                       then CEA-608/708 processing is disabled).\n");
	mprint("\n");

	mprint("Transcript customizing options:\n");

	mprint("    --customtxt format: Use the passed format to customize the (Timed) Transcript\n");
	mprint("                       output. The format must be like this: 1100100 (7 digits).\n");
	mprint("                       These indicate whether the next things should be\n");
	mprint("                       displayed or not in the (timed) transcript. They\n");
	mprint("                       represent (in order): \n");
	mprint("                           - Display start time\n");
	mprint("                           - Display end time\n");
	mprint("                           - Display caption mode\n");
	mprint("                           - Display caption channel\n");
	mprint("                           - Use a relative timestamp ( relative to the sample)\n");
	mprint("                           - Display XDS info\n");
	mprint("                           - Use colors\n");
	mprint("                       Examples:\n");
	mprint("                       0000101 is the default setting for transcripts\n");
	mprint("                       1110101 is the default for timed transcripts\n");
	mprint("                       1111001 is the default setting for -ucla\n");
	mprint("                       Make sure you use this parameter after others that might\n");
	mprint("                       affect these settings (-out, -ucla, -xds, -txt, \n");
	mprint("                       --ttxt ...)\n");

	mprint("\n");

	mprint("Communication with other programs and console output:\n");

	mprint("   --gui-mode-reports: Report progress and interesting events to stderr\n");
	mprint("                       in a easy to parse format. This is intended to be\n");
	mprint("                       used by other programs. See docs directory for.\n");
	mprint("                       details.\n");
	mprint("    --no-progress-bar: Suppress the output of the progress bar\n");
	mprint("               --quiet: Don't write any message.\n");
	mprint("\n");
#ifdef ENABLE_SHARING
	mprint("Sharing extracted captions via TCP:\n");
	mprint("     --enable-sharing: Enables real-time sharing of extracted captions\n");
	mprint("        --sharing-url: Set url for sharing service in nanomsg format. Default: \"tcp://*:3269\"\n");
	mprint("\n");

	mprint("CCTranslate application integration:\n");
	mprint("           --translate: Enable Translation tool and set target languages\n");
	mprint("                       in csv format (e.g. --translate ru,fr,it\n");
	mprint("     --translate-auth: Set Translation Service authorization data to make translation possible\n");
	mprint("                       In case of Google Translate API - API Key\n");
#endif // ENABLE_SHARING
	mprint("Burned-in subtitle extraction:\n");
	mprint("         --hardsubx : Enable the burned-in subtitle extraction subsystem.\n");
	mprint("\n");
	mprint("                      NOTE: This is needed to use the below burned-in \n");
	mprint("                		    subtitle extractor options\n");
	mprint("\n");
	mprint("       --tickertext : Search for burned-in ticker text at the bottom of\n");
	mprint("                     the screen.\n");
	mprint("\n");
	mprint("         --ocr-mode : Set the OCR mode to either frame-wise, word-wise\n");
	mprint("                     or letter wise.\n");
	mprint("                     e.g. --ocr-mode frame (default), --ocr-mode word, \n");
	mprint("                     --ocr-mode letter\n");
	mprint("\n");
	mprint("         --subcolor : Specify the color of the subtitles\n");
	mprint("                     Possible values are in the set \n");
	mprint("                     {white,yellow,green,cyan,blue,magenta,red}.\n");
	mprint("                     Alternatively, a custom hue value between 1 and 360 \n");
	mprint("                     may also be specified.\n");
	mprint("                     e.g. --subcolor white or --subcolor 270 (for violet).\n");
	mprint("                     Refer to an HSV color chart for values.\n");
	mprint("\n");
	mprint(" --min-sub-duration : Specify the minimum duration that a subtitle line \n");
	mprint("                     must exist on the screen.\n");
	mprint("                     The value is specified in seconds.\n");
	mprint("                     A lower value gives better results, but takes more \n");
	mprint("                     processing time.\n");
	mprint("                     The recommended value is 0.5 (default).\n");
	mprint("                     e.g. --min-sub-duration 1.0 (for a duration of 1 second)\n");
	mprint("\n");
	mprint("   --detect-italics : Specify whether italics are to be detected from the \n");
	mprint("                     OCR text.\n");
	mprint("                     Italic detection automatically enforces the OCR mode \n");
	mprint("                     to be word-wise");
	mprint("\n");
	mprint("      --conf-thresh : Specify the classifier confidence threshold between\n");
	mprint("                      1 and 100.\n");
	mprint("                     Try and use a threshold which works for you if you get \n");
	mprint("                     a lot of garbage text.\n");
	mprint("                     e.g. --conf-thresh 50\n");
	mprint("\n");
	mprint(" --whiteness-thresh : For white subtitles only, specify the luminance \n");
	mprint("                     threshold between 1 and 100\n");
	mprint("                     This threshold is content dependent, and adjusting\n");
	mprint("                     values may give you better results\n");
	mprint("                     Recommended values are in the range 80 to 100.\n");
	mprint("                     The default value is 95\n");
	mprint("\n");
	mprint("		--hcc	   : This option will be used if the file should have both\n");
	mprint("					 closed captions and burned in subtitles\n");
	mprint("            An example command for burned-in subtitle extraction is as follows:\n");
	mprint("               ccextractor video.mp4 --hardsubx -subcolor white --detect-italics \n");
	mprint("                   --whiteness-thresh 90 --conf-thresh 60\n");
	mprint("\n");
	mprint("\n         --version : Display current CCExtractor version and detailed information.\n");
	mprint("\n");
	mprint("Notes on File name related options:\n");
	mprint("  You can pass as many input files as you need. They will be processed in order.\n");
	mprint("  If a file name is suffixed by +, ccextractor will try to follow a numerical\n");
	mprint("  sequence. For example, DVD001.VOB+ means DVD001.VOB, DVD002.VOB and so on\n");
	mprint("  until there are no more files.\n");
	mprint("  Output will be one single file (either raw or srt). Use this if you made your\n");
	mprint("  recording in several cuts (to skip commercials for example) but you want one\n");
	mprint("  subtitle file with contiguous timing.\n");
	mprint("\n");
	mprint("Notes on Options that affect what will be processed:\n");
	mprint("  In general, if you want English subtitles you don't need to use these options\n");
	mprint("  as they are broadcast in field 1, channel 1. If you want the second language\n");
	mprint("  (usually Spanish) you may need to try -2, or -cc2, or both.\n");
	mprint("\n");
	mprint("Notes on Levenshtein distance:\n");
	mprint("  When processing teletext files CCExtractor tries to correct typos by\n");
	mprint("  comparing consecutive lines. If line N+1 is almost identical to line N except\n");
	mprint("  for minor changes (plus next characters) then it assumes that line N that a\n");
	mprint("  typo that was corrected in N+1. This is currently implemented in teletext\n");
	mprint("  because it's where samples files that could benefit from this were available.\n");
	mprint("  You can adjust, or disable, the algorithm settings with the following\n");
	mprint("  parameters.\n");
	mprint("\n");
	mprint("Notes on times:\n"
	       "  --startat and --endat times are used first, then -delay.\n");
	mprint("  So if you use --srt -startat 3:00 --endat 5:00 --delay 120000, ccextractor will\n");
	mprint("  generate a .srt file, with only data from 3:00 to 5:00 in the input file(s)\n");
	mprint("  and then add that (huge) delay, which would make the final file start at\n");
	mprint("  5:00 and end at 7:00.\n");
	mprint("\n");
	mprint("Notes on codec options:\n");
	mprint("  If codec type is not selected then first elementary stream suitable for \n"
	       "  subtitle is selected, please consider --teletext -noteletext override this\n"
	       "  option.\n");
	mprint("  no-codec and codec parameter must not be same if found to be same \n"
	       "  then parameter of no-codec is ignored, this flag should be passed \n"
	       "  once, more then one are not supported yet and last parameter would \n"
	       "  taken in consideration\n");
	mprint("\n");
	mprint("Notes on adding credits:\n");
	mprint("  CCExtractor can _try_ to add a custom message (for credits for example) at\n");
	mprint("  the start and end of the file, looking for a window where there are no\n");
	mprint("  captions. If there is no such window, then no text will be added.\n");
	mprint("  The start window must be between the times given and must have enough time\n");
	mprint("  to display the message for at least the specified time.\n");
	mprint("\n");
	mprint("Notes on the CEA-708 decoder:\n"
	       "  While it is starting to be useful, it's\n");
	mprint("  a work in progress. A number of things don't work yet in the decoder\n");
	mprint("  itself, and many of the auxiliary tools (case conversion to name one)\n");
	mprint("  won't do anything yet. Feel free to submit samples that cause problems\n");
	mprint("  and feature requests.\n");
	mprint("\n");
	mprint("Notes on spupng output format:\n");
	mprint("  One .xml file is created per output field. A set of .png files are created in\n");
	mprint("  a directory with the same base name as the corresponding .xml file(s), but with\n");
	mprint("  a .d extension. Each .png file will contain an image representing one caption\n");
	mprint("  and named subNNNN.png, starting with sub0000.png.\n");
	mprint("  For example, the command:\n");
	mprint("      ccextractor --out=spupng input.mpg\n");
	mprint("  will create the files:\n");
	mprint("      input.xml\n");
	mprint("      input.d/sub0000.png\n");
	mprint("      input.d/sub0001.png\n");
	mprint("      ...\n");
	mprint("  The command:\n");
	mprint("      ccextractor --out=spupng -o /tmp/output --12 input.mpg\n");
	mprint("  will create the files:\n");
	mprint("      /tmp/output_1.xml\n");
	mprint("      /tmp/output_1.d/sub0000.png\n");
	mprint("      /tmp/output_1.d/sub0001.png\n");
	mprint("      ...\n");
	mprint("      /tmp/output_2.xml\n");
	mprint("      /tmp/output_2.d/sub0000.png\n");
	mprint("      /tmp/output_2.d/sub0001.png\n");
	mprint("      ...");
	mprint("\n");
}

unsigned char sha256_buf[16384];

char *calculateSHA256(char *location)
{
	int size_read, bytes_read, fh = 0;
	SHA256_CTX ctx256;

	CC_SHA256_Init(&ctx256);

#ifdef _WIN32
	fh = OPEN(location, O_RDONLY | O_BINARY);
#else
	fh = OPEN(location, O_RDONLY);
#endif

	if (fh < 0)
	{
		return "Could not open file";
	}
	size_read = 0;
	while ((bytes_read = read(fh, sha256_buf, 16384)) > 0)
	{
		size_read += bytes_read;
		CC_SHA256_Update(&ctx256, (unsigned char *)sha256_buf, bytes_read);
	}
	close(fh);
	SHA256_End(&ctx256, sha256_buf);
	return sha256_buf;
}

void version(char *location)
{
	char *hash = calculateSHA256(location);
	mprint("CCExtractor detailed version info\n");
	mprint("	Version: %s\n", VERSION);
	mprint("	Git commit: %s\n", GIT_COMMIT);
	mprint("	Compilation date: %s\n", COMPILE_DATE);
#ifndef DISABLE_RUST
	mprint("	CEA-708 decoder: Rust\n");
#else
	mprint("	CEA-708 decoder: C\n");
#endif
	mprint("	File SHA256: %s\n", hash);

	mprint("Libraries used by CCExtractor\n");
#ifdef ENABLE_OCR
	mprint("	Tesseract Version: %s\n", (const char *)TessVersion());
	char *leptversion = getLeptonicaVersion();
	mprint("	Leptonica Version: %s\n", leptversion);
	lept_free(leptversion);
#endif // ENABLE_OCR
	mprint("	libGPAC Version: %s\n", GPAC_VERSION);
	mprint("	zlib: %s\n", ZLIB_VERSION);
	mprint("	utf8proc Version: %s\n", (const char *)utf8proc_version());
	mprint("	protobuf-c Version: %s\n", (const char *)protobuf_c_version());
	mprint("	libpng Version: %s\n", PNG_LIBPNG_VER_STRING);
	mprint("	FreeType \n");
	mprint("	libhash\n");
	mprint("	nuklear\n");
	mprint("	libzvbi\n");
}

void parse_708_services(struct ccx_s_options *opts, char *s)
{
	const char *all = "all";
	size_t all_len = strlen(all);
	int diff = strncmp(s, all, all_len);
	if (diff == 0)
	{
		size_t s_len = strlen(s);
		char *charset = NULL;
		if (s_len > all_len + 2) // '[' and ']'
			charset = strndup(s + all_len + 1, s_len - all_len - 2);

		opts->settings_dtvcc.enabled = 1;
		opts->enc_cfg.dtvcc_extract = 1;
		opts->enc_cfg.all_services_charset = charset;

		opts->enc_cfg.services_charsets = (char **)calloc(sizeof(char *), CCX_DTVCC_MAX_SERVICES);
		if (!opts->enc_cfg.services_charsets)
			ccx_common_logging.fatal_ftn(EXIT_NOT_ENOUGH_MEMORY, "parse_708_services");
		memset(opts->enc_cfg.services_charsets, 0, CCX_DTVCC_MAX_SERVICES * sizeof(char *));

		for (int i = 0; i < CCX_DTVCC_MAX_SERVICES; i++)
		{
			opts->settings_dtvcc.services_enabled[i] = 1;
			opts->enc_cfg.services_enabled[i] = 1;
		}

		opts->settings_dtvcc.active_services_count = CCX_DTVCC_MAX_SERVICES;
		return;
	}

	char *c, *e, *l;
	if (s == NULL)
		return;
	l = s + strlen(s);
	for (c = s; c < l && *c;)
	{
		int svc = -1;
		while (*c && !isdigit(*c))
			c++;
		if (!*c) // We're done
			break;
		e = c;
		while (isdigit(*e))
			e++;
		int charset_start_found = (*e == '[');
		*e = 0;
		svc = atoi(c);
		if (svc < 1 || svc > CCX_DTVCC_MAX_SERVICES)
			fatal(EXIT_MALFORMED_PARAMETER,
			      "[CEA-708] Malformed parameter: Invalid service number (%d), valid range is 1-%d.\n", svc, CCX_DTVCC_MAX_SERVICES);
		opts->settings_dtvcc.services_enabled[svc - 1] = 1;
		opts->enc_cfg.services_enabled[svc - 1] = 1;
		opts->settings_dtvcc.enabled = 1;
		opts->enc_cfg.dtvcc_extract = 1;
		opts->settings_dtvcc.active_services_count++;

		if (!opts->enc_cfg.services_charsets)
		{
			opts->enc_cfg.services_charsets = (char **)calloc(sizeof(char *), CCX_DTVCC_MAX_SERVICES);
			if (!opts->enc_cfg.services_charsets)
				ccx_common_logging.fatal_ftn(EXIT_NOT_ENOUGH_MEMORY, "parse_708_services\n");
			memset(opts->enc_cfg.services_charsets, 0, CCX_DTVCC_MAX_SERVICES * sizeof(char *));
		}

		e = e + 1;
		c = e;

		if (!charset_start_found)
			continue;

		while (*e && *e != ']' && *e != ',')
			e++;
		if (*e == ']')
		{
			char *charset = strndup(c, e - c);
			if (strlen(charset))
				opts->enc_cfg.services_charsets[svc - 1] = charset;
			c = e + 1;
		}
		else if (!*e)
		{
			fatal(EXIT_MALFORMED_PARAMETER, "[CEA-708] Malformed parameter: missing closing ] in CEA-708 services list\n");
		}
	}
	if (opts->settings_dtvcc.active_services_count <= 0)
		fatal(EXIT_MALFORMED_PARAMETER, "[CEA-708] Malformed parameter: no services\n");
}

long atol_size(char *s)
{
	long val = atoi(s);
	if (toupper(s[strlen(s) - 1]) == 'M')
		val *= 1024 * 1024;
	else if (toupper(s[strlen(s) - 1]) == 'K')
		val *= 1024;
	return val;
}

int atoi_hex(char *s)
{
	if (strlen(s) > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
	{
		// Hexadecimal
		return strtol(s + 2, NULL, 16);
	}
	else
	{
		return atoi(s);
	}
}

void mkvlang_params_check(char *lang)
{
	int initial = 0, present = 0;
	for (int char_index = 0; char_index < strlen(lang); char_index++)
	{
		lang[char_index] = cctolower(lang[char_index]);
		if (lang[char_index] == ',')
		{
			present = char_index;
			if ((present - initial < 6) && (present - initial != 3))
				fatal(EXIT_MALFORMED_PARAMETER, "language codes should be xxx,xxx,xxx,....\n");

			else if ((present - initial > 3) && (present - initial != 6))
				fatal(EXIT_MALFORMED_PARAMETER, "language codes should be xxx-xx,xxx-xx,xxx-xx,....\n");

			if ((present - initial > 3) && (present - initial == 6))
			{
				size_t length = present - initial;
				char *block = calloc(length + 1, sizeof(char));
				strncpy(block, lang + initial, length);
				char *hiphen_pointer = strstr(block, "-");
				if (!hiphen_pointer)
					fatal(EXIT_MALFORMED_PARAMETER, "language code is not of the form xxx-xx\n");
				free(block);
			}
			initial = present + 1;
		}
	}

	// Steps to check for the last lang of multiple mkvlangs provided by the user.
	present = strlen(lang) - 1;

	for (int char_index = strlen(lang) - 1; char_index >= 0; char_index--)
		if (lang[char_index] == ',')
		{
			initial = char_index + 1;
			break;
		}

	if ((present - initial < 5) && (present - initial != 2))
		fatal(EXIT_MALFORMED_PARAMETER, "last language code should be xxx.\n");

	else if ((present - initial > 2) && (present - initial != 5))
		fatal(EXIT_MALFORMED_PARAMETER, "last language code should be xxx-xx.\n");

	if ((present - initial > 2) && (present - initial == 5))
	{
		size_t length = present - initial;
		char *block = calloc(length + 1, sizeof(char));
		strncpy(block, lang + initial, length);
		char *hiphen_pointer = strstr(block, "-");
		if (!hiphen_pointer)
			fatal(EXIT_MALFORMED_PARAMETER, "last language code is not of the form xxx-xx\n");
		free(block);
	}
}

int parse_parameters(struct ccx_s_options *opt, int argc, char *argv[])
{
	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
		{
			print_usage();
			return EXIT_WITH_HELP;
		}
		if (strcmp(argv[i], "--version") == 0)
		{
			version(argv[0]);
			return EXIT_WITH_HELP;
		}
		if (strcmp(argv[i], "-") == 0 || strcmp(argv[i], "--stdin") == 0)
		{
			set_binary_mode();

			opt->input_source = CCX_DS_STDIN;
			if (!opt->live_stream)
				opt->live_stream = -1;
			continue;
		}
		if (argv[i][0] != '-')
		{
			int rc;
			if (argv[i][strlen(argv[i]) - 1] != '+')
			{
				rc = append_file_to_queue(opt, argv[i]);
			}
			else
			{
				argv[i][strlen(argv[i]) - 1] = 0;
				rc = add_file_sequence(opt, argv[i]);
			}
			if (rc)
			{
				fatal(EXIT_NOT_ENOUGH_MEMORY, "Fatal: Not enough memory to parse parameters.\n");
			}
			continue;
		}

#ifdef ENABLE_HARDSUBX
		// Parse --hardsubx and related parameters
		if (strcmp(argv[i], "--hardsubx") == 0)
		{
			opt->hardsubx = 1;
			continue;
		}
		if (opt->hardsubx == 1)
		{
			if (strcmp(argv[i], "--hcc") == 0)
			{
				// if extraction of both burned in and non burned in subs
				opt->hardsubx_and_common = 1;
				continue;
			}
			if (strcmp(argv[i], "--ocr-mode") == 0)
			{
				if (i < argc - 1)
				{
					i++;

					if (strcmp(argv[i], "simple") == 0 || strcmp(argv[i], "frame") == 0)
					{
						opt->hardsubx_ocr_mode = HARDSUBX_OCRMODE_FRAME;
					}
					else if (strcmp(argv[i], "word") == 0)
					{
						opt->hardsubx_ocr_mode = HARDSUBX_OCRMODE_WORD;
					}
					else if (strcmp(argv[i], "letter") == 0 || strcmp(argv[i], "symbol") == 0)
					{
						opt->hardsubx_ocr_mode = HARDSUBX_OCRMODE_LETTER;
					}
					else
					{
						fatal(EXIT_MALFORMED_PARAMETER, "-ocr_mode has an invalid value.\nValid values are {frame,word,letter}\n");
					}

					continue;
				}
				else
				{
					fatal(EXIT_MALFORMED_PARAMETER, "-ocr_mode has no argument.\nValid values are {frame,word,letter}\n");
				}
			}
			if (strcmp(argv[i], "--subcolor") == 0)
			{
				if (i < argc - 1)
				{
					i++;

					if (strcmp(argv[i], "white") == 0)
					{
						opt->hardsubx_subcolor = HARDSUBX_COLOR_WHITE;
						opt->hardsubx_hue = 0.0;
					}
					else if (strcmp(argv[i], "yellow") == 0)
					{
						opt->hardsubx_subcolor = HARDSUBX_COLOR_YELLOW;
						opt->hardsubx_hue = 60.0;
					}
					else if (strcmp(argv[i], "green") == 0)
					{
						opt->hardsubx_subcolor = HARDSUBX_COLOR_GREEN;
						opt->hardsubx_hue = 120.0;
					}
					else if (strcmp(argv[i], "cyan") == 0)
					{
						opt->hardsubx_subcolor = HARDSUBX_COLOR_CYAN;
						opt->hardsubx_hue = 180.0;
					}
					else if (strcmp(argv[i], "blue") == 0)
					{
						opt->hardsubx_subcolor = HARDSUBX_COLOR_BLUE;
						opt->hardsubx_hue = 240.0;
					}
					else if (strcmp(argv[i], "magenta") == 0)
					{
						opt->hardsubx_subcolor = HARDSUBX_COLOR_MAGENTA;
						opt->hardsubx_hue = 300.0;
					}
					else if (strcmp(argv[i], "red") == 0)
					{
						opt->hardsubx_subcolor = HARDSUBX_COLOR_RED;
						opt->hardsubx_hue = 0.0;
					}
					else
					{
						// Take a custom hue from the user
						opt->hardsubx_subcolor = HARDSUBX_COLOR_CUSTOM;
						char *str = (char *)malloc(sizeof(argv[i]));
						sprintf(str, "%s", argv[i]); // Done this way to avoid error with getting (i+1)th env variable
						opt->hardsubx_hue = atof(str);
						if (opt->hardsubx_hue <= 0.0 || opt->hardsubx_hue > 360.0)
						{
							fatal(EXIT_MALFORMED_PARAMETER, "--subcolor has either 0 or an invalid hue value supplied.\nIf you want to detect red subtitles, pass '-subcolor red' or a slightly higher hue value (e.g. 0.1)\n");
						}
					}

					continue;
				}
				else
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--subcolor has no argument.\nValid values are {white,yellow,green,cyan,blue,magenta,red} or a custom hue value between 0 and 360\n");
				}
			}
			if (strcmp(argv[i], "--min-sub-duration") == 0)
			{
				if (i < argc - 1)
				{
					i++;

					char *str = (char *)malloc(sizeof(argv[i]));
					sprintf(str, "%s", argv[i]); // Done this way to avoid error with getting (i+1)th env variable
					opt->hardsubx_min_sub_duration = atof(str);
					if (opt->hardsubx_min_sub_duration == 0.0)
					{
						fatal(EXIT_MALFORMED_PARAMETER, "--min-sub-duration has either 0 or an invalid value supplied\n");
					}

					continue;
				}
				else
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--min-sub-duration has no argument.\n");
				}
			}
			if (strcmp(argv[i], "--detect-italics") == 0)
			{
				opt->hardsubx_detect_italics = 1;
				continue;
			}
			if (strcmp(argv[i], "--conf-thresh") == 0)
			{
				if (i < argc - 1)
				{
					i++;

					char *str = (char *)malloc(sizeof(argv[i]));
					sprintf(str, "%s", argv[i]); // Done this way to avoid error with getting (i+1)th env variable
					opt->hardsubx_conf_thresh = atof(str);
					if (opt->hardsubx_conf_thresh <= 0.0 || opt->hardsubx_conf_thresh > 100.0)
					{
						fatal(EXIT_MALFORMED_PARAMETER, "--conf-thresh has either 0 or an invalid value supplied\nValid values are in (0.0,100.0)\n");
					}

					continue;
				}
				else
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--conf-thresh has no argument.\n");
				}
			}
			if (strcmp(argv[i], "--whiteness-thresh") == 0 || strcmp(argv[i], "--lum-thresh") == 0)
			{
				if (i < argc - 1)
				{
					i++;

					char *str = (char *)malloc(sizeof(argv[i]));
					sprintf(str, "%s", argv[i]); // Done this way to avoid error with getting (i+1)th env variable
					opt->hardsubx_lum_thresh = atof(str);
					if (opt->hardsubx_lum_thresh <= 0.0 || opt->hardsubx_conf_thresh > 100.0)
					{
						fatal(EXIT_MALFORMED_PARAMETER, "--whiteness-thresh has either 0 or an invalid value supplied\nValid values are in (0.0,100.0)\n");
					}

					continue;
				}
				else
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--whiteness-thresh has no argument.\n");
				}
			}
		}
#endif // ENABLE_HARDSUBX

		if (strcmp(argv[i], "--chapters") == 0)
		{
			opt->extract_chapters = 1;
			continue;
		}
		if (strcmp(argv[i], "--bufferinput") == 0)
		{
			opt->buffer_input = 1;
			continue;
		}
		if (strcmp(argv[i], "--no-bufferinput") == 0)
		{
			opt->buffer_input = 0;
			continue;
		}
		if (strcmp(argv[i], "--koc") == 0)
		{
			opt->keep_output_closed = 1;
			continue;
		}
		if (strcmp(argv[i], "--forceflush") == 0)
		{
			opt->force_flush = 1;
			continue;
		}
		if (strcmp(argv[i], "--append") == 0)
		{
			opt->append_mode = 1;
			continue;
		}
		if (strcmp(argv[i], "--buffersize") == 0)
		{
			if (i < argc - 1)
			{
				i++;

				FILEBUFFERSIZE = atol_size(argv[i]);
				if (FILEBUFFERSIZE < 8)
				{
					FILEBUFFERSIZE = 8; // Otherwise crashes are guaranteed at least in MythTV
				}

				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--buffersize has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--dru") == 0)
		{
			opt->settings_608.direct_rollup = 1;
			continue;
		}
		if (strcmp(argv[i], "--no-fontcolor") == 0)
		{
			opt->nofontcolor = 1;
			continue;
		}
		if (strcmp(argv[i], "--no-htmlescape") == 0)
		{
			opt->nohtmlescape = 1;
			continue;
		}
		if (strcmp(argv[i], "--bom") == 0)
		{
			opt->enc_cfg.no_bom = 0;
			continue;
		}
		if (strcmp(argv[i], "--no-bom") == 0)
		{
			opt->enc_cfg.no_bom = 1;
			continue;
		}
		if (strcmp(argv[i], "--sem") == 0)
		{
			opt->enc_cfg.with_semaphore = 1;
			continue;
		}
		if (strcmp(argv[i], "--no-typesetting") == 0)
		{
			opt->notypesetting = 1;
			continue;
		}

		if (strcmp(argv[i], "--timestamp-map") == 0)
		{
			opt->timestamp_map = 1;
			continue;
		}

		/* Input file formats */
		if (strcmp(argv[i], "--es") == 0 ||
		    strcmp(argv[i], "--ts") == 0 ||
		    strcmp(argv[i], "--ps") == 0 ||
		    strcmp(argv[i], "--asf") == 0 ||
		    strcmp(argv[i], "--wtv") == 0 ||
		    strcmp(argv[i], "--mp4") == 0 ||
		    strcmp(argv[i], "--mkv") == 0 ||
		    strcmp(argv[i], "--dvr-ms") == 0)
		{
			set_input_format(opt, argv[i]);
			continue;
		}
		if (strncmp(argv[i], "--in=", 5) == 0)
		{
			set_input_format(opt, argv[i] + 5);
			continue;
		}

		/*user specified subtitle to be selected */
		if (strcmp(argv[i], "--codec") == 0)
		{
			if (i < argc - 1)
			{
				i++;

				if (strcmp(argv[i], "teletext") == 0)
				{
					opt->demux_cfg.codec = CCX_CODEC_TELETEXT;
				}
				else if (strcmp(argv[i], "dvbsub") == 0)
				{
					opt->demux_cfg.codec = CCX_CODEC_DVB;
				}
				else
				{
					fatal(EXIT_MALFORMED_PARAMETER, "Invalid option for codec.\n");
				}

				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--codec has no argument.\n");
			}
		}

		/*user specified subtitle to be selected */
		if (strcmp(argv[i], "--no-codec") == 0)
		{
			if (i < argc - 1)
			{
				i++;

				if (strcmp(argv[i], "teletext") == 0)
				{
					opt->demux_cfg.nocodec = CCX_CODEC_TELETEXT;
				}
				else if (strcmp(argv[i], "dvbsub") == 0)
				{
					opt->demux_cfg.nocodec = CCX_CODEC_DVB;
				}
				else
				{
					fatal(EXIT_MALFORMED_PARAMETER, "Invalid option for no-codec.\n");
				}

				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--no-codec has no argument.\n");
			}
		}

		if (strcmp(argv[i], "--dvblang") == 0)
		{
			if (i < argc - 1)
			{
				i++;

				opt->dvblang = (char *)malloc(sizeof(argv[i]));
				sprintf(opt->dvblang, "%s", argv[i]);
				for (int char_index = 0; char_index < strlen(opt->dvblang); char_index++)
				{
					opt->dvblang[char_index] = cctolower(opt->dvblang[char_index]);
				}

				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--dvblang has no argument.\n");
			}
		}

		if (strcmp(argv[i], "--ocrlang") == 0)
		{
			if (i++ < argc - 1)
			{
				opt->ocrlang = argv[i];
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--ocrlang has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--quant") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->ocr_quantmode = atoi(argv[i]);
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--quant has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--no-spupngocr") == 0)
		{
			opt->enc_cfg.nospupngocr = 1;
			continue;
		}
		if (strcmp(argv[i], "--oem") == 0)
		{
			if (i < argc - 1)
			{
				i++;

				char *str = (char *)malloc(sizeof(argv[i]));
				sprintf(str, "%s", argv[i]);
				opt->ocr_oem = atoi(str);
				if (opt->ocr_oem < 0 || opt->ocr_oem > 2)
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--oem must be 0, 1 or 2\n");
				}

				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--oem has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--mkvlang") == 0)
		{
			if (i < argc - 1)
			{
				i++;

				opt->mkvlang = (char *)malloc(sizeof(argv[i]));
				sprintf(opt->mkvlang, "%s", argv[i]);
				mkvlang_params_check(opt->mkvlang);

				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--mkvlang has no argument.\n");
			}
		}

		/* Output file formats */
		if (strcmp(argv[i], "--srt") == 0 ||
		    strcmp(argv[i], "--mcc") == 0 || strcmp(argv[i], "--dvdraw") == 0 ||
		    strcmp(argv[i], "--smi") == 0 || strcmp(argv[i], "--sami") == 0 ||
		    strcmp(argv[i], "--txt") == 0 || strcmp(argv[i], "--transcript") == 0 ||
		    strcmp(argv[i], "--ttxt") == 0 || strcmp(argv[i], "--timedtranscript") == 0 ||
		    strcmp(argv[i], "--webvtt") == 0 || strcmp(argv[i], "--null") == 0)
		{
			set_output_format(opt, argv[i]);
			continue;
		}
		if (strncmp(argv[i], "--out=", 6) == 0)
		{
			set_output_format(opt, argv[i] + 6);
			continue;
		}

		/* Credit stuff */
		if (strcmp(argv[i], "--startcreditstext") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->enc_cfg.start_credits_text = argv[i];
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--startcreditstext has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--startcreditsnotbefore") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				if (stringztoms(argv[i], &opt->enc_cfg.startcreditsnotbefore) == -1)
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--startcreditsnotbefore only accepts SS, MM:SS or HH:MM:SS\n");
				}
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--startcreditsnotbefore has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--startcreditsnotafter") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				if (stringztoms(argv[i], &opt->enc_cfg.startcreditsnotafter) == -1)
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--startcreditsnotafter only accepts SS, MM:SS or HH:MM:SS\n");
				}
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--startcreditsnotafter has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--startcreditsforatleast") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				if (stringztoms(argv[i], &opt->enc_cfg.startcreditsforatleast) == -1)
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--startcreditsforatleast only accepts SS, MM:SS or HH:MM:SS\n");
				}
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--startcreditsforatleast has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--startcreditsforatmost") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				if (stringztoms(argv[i], &opt->enc_cfg.startcreditsforatmost) == -1)
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--startcreditsforatmost only accepts SS, MM:SS or HH:MM:SS\n");
				}
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--startcreditsforatmost has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--endcreditstext") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->enc_cfg.end_credits_text = argv[i];
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--endcreditstext has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--endcreditsforatleast") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				if (stringztoms(argv[i], &opt->enc_cfg.endcreditsforatleast) == -1)
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--endcreditsforatleast only accepts SS, MM:SS or HH:MM:SS\n");
				}
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--endcreditsforatleast has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--endcreditsforatmost") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				if (stringztoms(argv[i], &opt->enc_cfg.endcreditsforatmost) == -1)
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--startcreditsforatmost only accepts SS, MM:SS or HH:MM:SS\n");
				}
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--endcreditsforatmost has no argument.\n");
			}
		}

		/* More stuff */
		if (strcmp(argv[i], "--videoedited") == 0)
		{
			opt->binary_concat = 0;
			continue;
		}
		if (strcmp(argv[i], "--goptime") == 0)
		{
			opt->use_gop_as_pts = 1;
			continue;
		}
		if (strcmp(argv[i], "--no-goptime") == 0)
		{
			opt->use_gop_as_pts = -1; // Don't use even if we would want to
			continue;
		}
		if (strcmp(argv[i], "--fixpadding") == 0)
		{
			opt->fix_padding = 1;
			continue;
		}
		if (strcmp(argv[i], "--90090") == 0)
		{
			MPEG_CLOCK_FREQ = 90090;
			continue;
		}
		if (strcmp(argv[i], "--no-scte20") == 0)
		{
			opt->noscte20 = 1;
			continue;
		}
		if (strcmp(argv[i], "--webvtt-create-css") == 0)
		{
			opt->webvtt_create_css = 1;
			continue;
		}
		if (strcmp(argv[i], "--no-rollup") == 0)
		{
			opt->no_rollup = 1;
			opt->settings_608.no_rollup = 1;
			opt->settings_dtvcc.no_rollup = 1;
			continue;
		}
		if (strcmp(argv[i], "--ru1") == 0)
		{
			opt->settings_608.force_rollup = 1;
			continue;
		}
		if (strcmp(argv[i], "--ru2") == 0)
		{
			opt->settings_608.force_rollup = 2;
			continue;
		}
		if (strcmp(argv[i], "--ru3") == 0)
		{
			opt->settings_608.force_rollup = 3;
			continue;
		}
		if (strcmp(argv[i], "--trim") == 0)
		{
			opt->enc_cfg.trim_subs = 1;
			continue;
		}
		if (strcmp(argv[i], "--outinterval") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->out_interval = atoi(argv[i]);
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "-outinterval has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--segmentonkeyonly") == 0)
		{
			opt->segment_on_key_frames_only = 1;
			opt->analyze_video_stream = 1;
			continue;
		}
		if (strcmp(argv[i], "--gui-mode-reports") == 0)
		{
			opt->gui_mode_reports = 1;
			continue;
		}
		if (strcmp(argv[i], "--no-progress-bar") == 0)
		{
			opt->no_progress_bar = 1;
			continue;
		}
		if (strcmp(argv[i], "--splitbysentence") == 0)
		{
			opt->enc_cfg.splitbysentence = 1;
			continue;
		}

		if (strcmp(argv[i], "--sentencecap") == 0)
		{
			opt->enc_cfg.sentence_cap = 1;
			continue;
		}

		if (strcmp(argv[i], "--capfile") == 0 && i < argc - 1)
		{
			opt->enc_cfg.sentence_cap = 1;
			opt->sentence_cap_file = argv[i + 1];
			i++;
			continue;
		}

		if (strcmp(argv[i], "--kf") == 0) // Kid friendly (removes profanity)
		{
			opt->enc_cfg.filter_profanity = 1;
			continue;
		}

		if (strcmp(argv[i], "--profanity-file") == 0)
		{
			// TODO: decide whether this is really wanted. A script running ccextractor
			// could want to pass this argument at all time and only use it with --kf
			// Issue also applies to --capfile
			opt->enc_cfg.filter_profanity = 1;
			opt->filter_profanity_file = argv[++i];
			continue;
		}

		if (strcmp(argv[i], "--program-number") == 0)
		{
			if (i < argc - 1 && isanumber(argv[i + 1]))
			{
				i++;
				opt->demux_cfg.ts_forced_program = atoi_hex(argv[i]);
				opt->demux_cfg.ts_forced_program_selected = 1;
				continue;
			}
			else
			{
				opt->demux_cfg.ts_forced_program = -1; // Autodetect
				continue;
			}
		}
		if (strcmp(argv[i], "--autoprogram") == 0)
		{
			opt->demux_cfg.ts_autoprogram = 1;
			continue;
		}
		if (strcmp(argv[i], "--multiprogram") == 0)
		{
			opt->multiprogram = 1;
			opt->demux_cfg.ts_allprogram = CCX_TRUE;
			continue;
		}
		if (strcmp(argv[i], "--stream") == 0 || strcmp(argv[i], "-s") == 0)
		{
			if (i < argc - 1 && isanumber(argv[i + 1]))
			{
				i++;
				opt->live_stream = atoi_hex(argv[i]);
				continue;
			}
			else
			{
				opt->live_stream = -1; // Live stream without timeout
				continue;
			}
		}
		if (strcmp(argv[i], "--defaultcolor") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				if (strlen(argv[i]) != 7 || argv[i][0] != '#')
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--defaultcolor expects a 7 character parameter that starts with #\n");
				}
				strcpy((char *)usercolor_rgb, argv[i]);
				opt->settings_608.default_color = COL_USERDEFINED;
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--defaultcolor has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--delay") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				if (parsedelay(opt, argv[i]))
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--delay only accept integers (such as --300 or 300)\n");
				}
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--delay has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--screenfuls") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->settings_608.screens_to_process = atoi_hex(argv[i]);
				if (opt->settings_608.screens_to_process < 0)
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--screenfuls only accepts non-negative integers.\n");
				}
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--screenfuls has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--startat") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				if (stringztoms(argv[i], &opt->extraction_start) == -1)
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--startat only accepts SS, MM:SS or HH:MM:SS\n");
				}
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--startat has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--endat") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				if (stringztoms(argv[i], &opt->extraction_end) == -1)
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--endat only accepts SS, MM:SS or HH:MM:SS\n");
				}
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--endat has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--output-field") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->extract = strcmp(argv[i], "both") == 0 ? 12 : atoi_hex(argv[i]);
				if (opt->extract != 1 && opt->extract != 2 && opt->extract != 12)
				{
					fatal(EXIT_MALFORMED_PARAMETER, "--output-field only accepts 1 or 2 or both.\n");
				}
				opt->is_608_enabled = 1;
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--output-field has no argument.\n");
			}
		}

		if (strcmp(argv[i], "--cc2") == 0 || strcmp(argv[i], "--CC2") == 0)
		{
			opt->cc_channel = 2;
			continue;
		}
		if (strcmp(argv[i], "--stdout") == 0)
		{
			if (opt->messages_target == 1) // Only change this if still stdout. --quiet could set it to 0 for example
			{
				opt->messages_target = 2; // stderr
			}
			opt->cc_to_stdout = 1;
			continue;
		}
		if (strcmp(argv[i], "--pesheader") == 0)
		{
			opt->pes_header_to_stdout = 1;
			continue;
		}
		if (strcmp(argv[i], "--debugdvbsub") == 0)
		{
			opt->debug_mask |= CCX_DMT_DVB;
			continue;
		}
		if (strcmp(argv[i], "--ignoreptsjumps") == 0)
		{
			opt->ignore_pts_jumps = 1;
			continue;
		}
		// --ignoreptsjumps counterpart
		if (strcmp(argv[i], "--fixptsjumps") == 0)
		{
			opt->ignore_pts_jumps = 0;
			continue;
		}
		if (strcmp(argv[i], "--quiet") == 0)
		{
			opt->messages_target = 0;
			continue;
		}
		if (strcmp(argv[i], "--debug") == 0)
		{
			opt->debug_mask |= CCX_DMT_VERBOSE;
			continue;
		}
		if (strcmp(argv[i], "--608") == 0)
		{
			opt->debug_mask |= CCX_DMT_DECODER_608;
			continue;
		}
		if (strcmp(argv[i], "--deblev") == 0)
		{
			opt->debug_mask |= CCX_DMT_LEVENSHTEIN;
			continue;
		}
		if (strcmp(argv[i], "--no-levdist") == 0)
		{
			opt->dolevdist = 0;
			continue;
		}
		if (strcmp(argv[i], "--levdistmincnt") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->levdistmincnt = atoi_hex(argv[i]);
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--levdistmincnt has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--levdistmaxpct") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->levdistmaxpct = atoi_hex(argv[i]);
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--levdistmaxpct has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--708") == 0)
		{
			opt->debug_mask |= CCX_DMT_708;
#ifndef DISABLE_RUST
			ccxr_init_logger();
#endif
			continue;
		}
		if (strcmp(argv[i], "--goppts") == 0)
		{
			opt->debug_mask |= CCX_DMT_TIME;
			continue;
		}
		if (strcmp(argv[i], "--vides") == 0)
		{
			opt->debug_mask |= CCX_DMT_VIDES;
			opt->analyze_video_stream = 1;
			continue;
		}
		if (strcmp(argv[i], "--analyzevideo") == 0)
		{
			opt->analyze_video_stream = 1;
			continue;
		}
		if (strcmp(argv[i], "--xds") == 0)
		{
			// XDS can be set regardless of --UCLA (isFinal) usage.
			opt->transcript_settings.xds = 1;
			continue;
		}
		if (strcmp(argv[i], "--xdsdebug") == 0)
		{
			opt->transcript_settings.xds = 1;
			opt->debug_mask |= CCX_DMT_DECODER_XDS;
			continue;
		}
		if (strcmp(argv[i], "--parsedebug") == 0)
		{
			opt->debug_mask |= CCX_DMT_PARSE;
			continue;
		}
		if (strcmp(argv[i], "--parsePAT") == 0 || strcmp(argv[i], "--parsepat") == 0)
		{
			opt->debug_mask |= CCX_DMT_PAT;
			continue;
		}
		if (strcmp(argv[i], "--parsePMT") == 0 || strcmp(argv[i], "--parsepmt") == 0)
		{
			opt->debug_mask |= CCX_DMT_PMT;
			continue;
		}
		if (strcmp(argv[i], "--dumpdef") == 0)
		{
			opt->debug_mask |= CCX_DMT_DUMPDEF;
			continue;
		}
		if (strcmp(argv[i], "--investigate-packets") == 0)
		{
			opt->investigate_packets = 1;
			continue;
		}
		if (strcmp(argv[i], "--cbraw") == 0)
		{
			opt->debug_mask |= CCX_DMT_CBRAW;
			continue;
		}
		if (strcmp(argv[i], "--tverbose") == 0)
		{
			opt->debug_mask |= CCX_DMT_TELETEXT;
			tlt_config.verbose = 1;
			continue;
		}
#ifdef ENABLE_SHARING
		if (strcmp(argv[i], "--sharing-debug") == 0)
		{
			opt->debug_mask |= CCX_DMT_SHARE;
			continue;
		}
#endif // ENABLE_SHARING
		if (strcmp(argv[i], "--fullbin") == 0)
		{
			opt->fullbin = 1;
			continue;
		}
		if (strcmp(argv[i], "--no-sync") == 0)
		{
			opt->nosync = 1;
			continue;
		}
		if (strcmp(argv[i], "--hauppauge") == 0)
		{
			opt->hauppauge_mode = 1;
			continue;
		}
		if (strcmp(argv[i], "--mp4vidtrack") == 0)
		{
			opt->mp4vidtrack = 1;
			continue;
		}
		if (strstr(argv[i], "--unicode") != NULL)
		{
			opt->enc_cfg.encoding = CCX_ENC_UNICODE;
			continue;
		}
		if (strstr(argv[i], "--utf8") != NULL)
		{
			opt->enc_cfg.encoding = CCX_ENC_UTF_8;
			continue;
		}
		if (strstr(argv[i], "--latin1") != NULL)
		{
			opt->enc_cfg.encoding = CCX_ENC_LATIN_1;
			continue;
		}
		if (strcmp(argv[i], "--usepicorder") == 0)
		{
			opt->usepicorder = 1;
			continue;
		}
		if (strstr(argv[i], "--myth") != NULL)
		{
			opt->auto_myth = 1;
			continue;
		}
		if (strstr(argv[i], "--no-myth") != NULL)
		{
			opt->auto_myth = 0;
			continue;
		}
		if (strstr(argv[i], "--wtvconvertfix") != NULL)
		{
			opt->wtvconvertfix = 1;
			continue;
		}
		if (strstr(argv[i], "--wtvmpeg2") != NULL)
		{
			opt->wtvmpeg2 = 1;
			continue;
		}
		if (strcmp(argv[i], "-o") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->output_filename = argv[i];
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "-o has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--service") == 0)
		{
			if (i < argc - 1)
			{
				opt->is_708_enabled = 1;
				i++;
				parse_708_services(opt, argv[i]);
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--service has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--datapid") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->demux_cfg.ts_cappids[opt->demux_cfg.nb_ts_cappid] = atoi_hex(argv[i]);
				opt->demux_cfg.nb_ts_cappid++;
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--datapid has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--datastreamtype") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->demux_cfg.ts_datastreamtype = atoi_hex(argv[i]);
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--datastreamtype has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--streamtype") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->demux_cfg.ts_forced_streamtype = atoi_hex(argv[i]);
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--streamtype has no argument.\n");
			}
		}

		/* Teletext stuff */
		if (strcmp(argv[i], "--tpage") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				tlt_config.page = atoi_hex(argv[i]);
				tlt_config.user_page = tlt_config.page;
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--tpage has no argument.\n");
			}
		}

		/* Red Hen/ UCLA Specific stuff */
		if (strcmp(argv[i], "--UCLA") == 0 || strcmp(argv[i], "--ucla") == 0)
		{
			opt->ucla = 1;
			opt->millis_separator = '.';
			opt->enc_cfg.no_bom = 1;
			if (!opt->transcript_settings.isFinal)
			{
				opt->transcript_settings.showStartTime = 1;
				opt->transcript_settings.showEndTime = 1;
				opt->transcript_settings.showCC = 1;
				opt->transcript_settings.showMode = 1;
				opt->transcript_settings.relativeTimestamp = 0;
				opt->transcript_settings.isFinal = 1;
			}
			continue;
		}
		if (strcmp(argv[i], "--latrusmap") == 0)
		{
			tlt_config.latrusmap = 1;
			continue;
		}
		if (strcmp(argv[i], "--tickertext") == 0 || strcmp(argv[i], "--tickertape") == 0)
		{
			opt->tickertext = 1;
			continue;
		}
		if (strcmp(argv[i], "--lf") == 0 || strcmp(argv[i], "--LF") == 0)
		{
			opt->enc_cfg.line_terminator_lf = 1;
			continue;
		}
		if (strcmp(argv[i], "--df") == 0 || strcmp(argv[i], "--DF") == 0)
		{
			opt->enc_cfg.force_dropframe = 1;
			continue;
		}
		if (strcmp(argv[i], "--no-autotimeref") == 0)
		{
			opt->noautotimeref = 1;
			continue;
		}
		if (strcmp(argv[i], "--autodash") == 0 || strcmp(argv[i], "--sem") == 0)
		{
			opt->enc_cfg.autodash = 1;
			continue;
		}
		if (strcmp(argv[i], "--xmltv") == 0)
		{
			if (i < argc - 1 && isanumber(argv[i + 1]))
			{
				i++;
				opt->xmltv = atoi_hex(argv[i]);
				continue;
			}
			else
			{
				opt->xmltv = 1;
				continue;
			}
		}
		if (strcmp(argv[i], "--xmltvliveinterval") == 0)
		{
			if (i < argc - 1 && isanumber(argv[i + 1]))
			{
				i++;
				opt->xmltvliveinterval = atoi_hex(argv[i]);
				continue;
			}
			else
			{
				opt->xmltvliveinterval = 10;
				continue;
			}
		}
		if (strcmp(argv[i], "--xmltvoutputinterval") == 0)
		{
			if (i < argc - 1 && isanumber(argv[i + 1]))
			{
				i++;
				opt->xmltvoutputinterval = atoi_hex(argv[i]);
				continue;
			}
			else
			{
				opt->xmltvoutputinterval = 0;
				continue;
			}
		}
		if (strcmp(argv[i], "--xmltvonlycurrent") == 0)
		{
			opt->xmltvonlycurrent = 1;
			i++; // why do we skip next?
			continue;
		}
		if (strcmp(argv[i], "--unixts") == 0)
		{
			if (i < argc - 1)
			{
				i++;

				uint64_t t = atoi_hex(argv[i]);
				if (t <= 0)
				{
					time_t now = time(NULL);
					t = time(&now);
				}
				utc_refvalue = t;
				opt->noautotimeref = 1; // If set by user don't attempt to fix

				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--unixts has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--sects") == 0)
		{
			opt->date_format = ODF_SECONDS;
			continue;
		}
		if (strcmp(argv[i], "--datets") == 0)
		{
			opt->date_format = ODF_DATE;
			continue;
		}
		if (strcmp(argv[i], "--teletext") == 0)
		{
			opt->demux_cfg.codec = CCX_CODEC_TELETEXT;
			continue;
		}
		if (strcmp(argv[i], "--no-teletext") == 0)
		{
			opt->demux_cfg.nocodec = CCX_CODEC_TELETEXT;
			continue;
		}
		/* Custom transcript */
		if (strcmp(argv[i], "--customtxt") == 0)
		{
			if (i < argc - 1)
			{
				i++;

				char *format = argv[i];
				if (strlen(format) == 7)
				{
					if (opt->date_format == ODF_NONE)
					{
						opt->date_format = ODF_HHMMSSMS; // Necessary for displaying times, if any would be used.
					}
					if (!opt->transcript_settings.isFinal)
					{
						opt->transcript_settings.showStartTime = format[0] - '0';
						opt->transcript_settings.showEndTime = format[1] - '0';
						opt->transcript_settings.showMode = format[2] - '0';
						opt->transcript_settings.showCC = format[3] - '0';
						opt->transcript_settings.relativeTimestamp = format[4] - '0';
						opt->transcript_settings.xds = format[5] - '0';
						opt->transcript_settings.useColors = format[6] - '0';
					}
					else
					{
						fatal(EXIT_INCOMPATIBLE_PARAMETERS, "customtxt cannot be set after --UCLA is used!\n");
					}
				}
				else
				{
					fatal(EXIT_MALFORMED_PARAMETER, "Custom TXT format not OK: %s, expected 7 bits string\n", format);
				}

				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--customtxt has no argument.\n");
			}
		}

		/* Network stuff */
		if (strcmp(argv[i], "--udp") == 0)
		{
			if (i < argc - 1)
			{
				i++;

				char *at = strchr(argv[i], '@');
				char *colon = strchr(argv[i], ':');
				if (at && !colon)
				{
					fatal(EXIT_MALFORMED_PARAMETER, "If --udp contains an '@', it must also contain a ':'\n");
				}
				else if (at && colon)
				{
					*at = '\0';
					*colon = '\0';
					opt->udpsrc = argv[i];
					opt->udpaddr = at + 1;
					opt->udpport = atoi_hex(colon + 1);
				}
				else if (colon)
				{
					*colon = '\0';
					opt->udpaddr = argv[i];
					opt->udpport = atoi_hex(colon + 1);
				}
				else
				{
					opt->udpaddr = NULL;
					opt->udpport = atoi_hex(argv[i]);
				}
				opt->input_source = CCX_DS_NETWORK;

				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--udp has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--sendto") == 0)
		{
			if (i < argc - 1)
			{
				i++;

				opt->send_to_srv = 1;

				set_output_format(opt, "bin");

				opt->xmltv = 2;
				opt->xmltvliveinterval = 2;

				char *addr = argv[i];
				if (*addr == '[')
				{
					addr++;

					opt->srv_addr = addr;

					char *br = strchr(addr, ']');
					if (br == NULL)
					{
						fatal(EXIT_INCOMPATIBLE_PARAMETERS, "Wrong address format, for IPv6 use [address]:port\n");
					}
					*br = '\0';

					br++; /* Colon */
					if (*br != '\0')
					{
						opt->srv_port = br + 1;
					}

					continue;
				}

				opt->srv_addr = argv[i];

				char *colon = strchr(argv[i], ':');
				if (colon != NULL)
				{
					*colon = '\0';
					opt->srv_port = colon + 1;
				}

				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--sendto has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--tcp") == 0)
		{
			if (i < argc - 1)
			{
				i++;

				opt->tcpport = argv[i];
				opt->input_source = CCX_DS_TCP;

				set_input_format(opt, "bin");

				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--tcp has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--tcp-password") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->tcp_password = argv[i];
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--tcp-password has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--tcp-description") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->tcp_desc = argv[i];
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--tcpdesc has no argument.\n");
			}
		}

		if (strcmp(argv[i], "--font") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->enc_cfg.render_font = argv[i];
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--font has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--italics") == 0 && i < argc - 1)
		{
			opt->enc_cfg.render_font_italics = argv[i + 1];
			i++;
			continue;
		}

#ifdef WITH_LIBCURL
		if (strcmp(argv[i], "--curlposturl") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->curlposturl = argv[i];
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--curlposturl has no argument.\n");
			}
		}
#endif // WITH_LIBCURL

#ifdef ENABLE_SHARING
		if (strcmp(argv[i], "--enable-sharing") == 0)
		{
			opt->sharing_enabled = 1;
			continue;
		}
		if (strcmp(argv[i], "--sharing-url") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->sharing_url = argv[i];
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--sharing-url has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--translate") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->translate_enabled = 1;
				opt->sharing_enabled = 1;
				opt->translate_langs = argv[i];
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--translate has no argument.\n");
			}
		}
		if (strcmp(argv[i], "--translate-auth") == 0)
		{
			if (i < argc - 1)
			{
				i++;
				opt->translate_key = argv[i];
				continue;
			}
			else
			{
				fatal(EXIT_MALFORMED_PARAMETER, "--translate-auth has no argument.\n");
			}
		}
#endif // ENABLE_SHARING

		fatal(EXIT_INCOMPATIBLE_PARAMETERS, "Error: Parameter %s not understood.\n", argv[i]);
	}

	if (opt->demux_cfg.auto_stream == CCX_SM_MP4 && opt->input_source == CCX_DS_STDIN)
	{
		fatal(EXIT_INCOMPATIBLE_PARAMETERS, "MP4 requires an actual file, it's not possible to read from a stream, including stdin.\n");
	}

	if (opt->extract_chapters)
	{
		mprint("Request to extract chapters recieved.\n");
		mprint("Note that this must only be used with MP4 files,\n");
		mprint("for other files it will simply generate subtitles file.\n\n");
	}

	if (opt->gui_mode_reports)
	{
		opt->no_progress_bar = 1;
		// Do it as soon as possible, because it something fails we might not have a chance
		activity_report_version();
	}

	if (opt->enc_cfg.sentence_cap)
	{
		if (add_builtin_words(capitalized_builtin, &capitalization_list))
			fatal(EXIT_NOT_ENOUGH_MEMORY, "Not enough memory for capitalized word list");
		if (opt->sentence_cap_file && process_word_file(opt->sentence_cap_file, &capitalization_list))
			fatal(EXIT_ERROR_IN_CAPITALIZATION_FILE, "There was an error processing the capitalization file.\n");
	}

	if (opt->enc_cfg.filter_profanity)
	{
		if (add_builtin_words(profane_builtin, &profane))
			fatal(EXIT_NOT_ENOUGH_MEMORY, "Not enough memory for profane word list");
		if (opt->filter_profanity_file && process_word_file(opt->filter_profanity_file, &profane))
			fatal(EXIT_ERROR_IN_CAPITALIZATION_FILE, "There was an error processing the profanity file.\n");
	}

	ccx_encoders_helpers_perform_shellsort_words(); // Sort the words for binary search for capitalization and profanity censorship

	if (opt->demux_cfg.ts_forced_program != -1)
		opt->demux_cfg.ts_forced_program_selected = 1;

	// Init telexcc redundant options
	tlt_config.dolevdist = opt->dolevdist;
	tlt_config.levdistmincnt = opt->levdistmincnt;
	tlt_config.levdistmaxpct = opt->levdistmaxpct;
	tlt_config.extraction_start = opt->extraction_start;
	tlt_config.extraction_end = opt->extraction_end;
	tlt_config.write_format = opt->write_format;
	tlt_config.gui_mode_reports = opt->gui_mode_reports;
	tlt_config.date_format = opt->date_format;
	tlt_config.noautotimeref = opt->noautotimeref;
	tlt_config.send_to_srv = opt->send_to_srv;
	tlt_config.nofontcolor = opt->nofontcolor;
	tlt_config.nohtmlescape = opt->nohtmlescape;
	tlt_config.millis_separator = opt->millis_separator;

	// teletext page number out of range
	if (tlt_config.page != 0 && (tlt_config.page < 100 || tlt_config.page > 899))
	{
		print_error(opt->gui_mode_reports, "Teletext page number could not be lower than 100 or higher than 899\n");
		return EXIT_NOT_CLASSIFIED;
	}

	if (opt->num_input_files == 0 && opt->input_source == CCX_DS_FILE)
	{
		return EXIT_NO_INPUT_FILES;
	}
	if (opt->num_input_files > 1 && opt->live_stream)
	{
		print_error(opt->gui_mode_reports, "Live stream mode accepts only one input file.\n");
		return EXIT_TOO_MANY_INPUT_FILES;
	}
	if (opt->num_input_files && opt->input_source == CCX_DS_NETWORK)
	{
		print_error(opt->gui_mode_reports, "UDP mode is not compatible with input files.\n");
		return EXIT_TOO_MANY_INPUT_FILES;
	}
	if (opt->input_source == CCX_DS_NETWORK || opt->input_source == CCX_DS_TCP)
	{
		ccx_options.buffer_input = 1; // Mandatory, because each datagram must be read complete.
	}
	if (opt->num_input_files && opt->input_source == CCX_DS_TCP)
	{
		print_error(opt->gui_mode_reports, "TCP mode is not compatible with input files.\n");
		return EXIT_TOO_MANY_INPUT_FILES;
	}

	if (opt->demux_cfg.auto_stream == CCX_SM_MCPOODLESRAW && opt->write_format == CCX_OF_RAW)
	{
		print_error(opt->gui_mode_reports, "-in=raw can only be used if the output is a subtitle file.\n");
		return EXIT_INCOMPATIBLE_PARAMETERS;
	}
	if (opt->demux_cfg.auto_stream == CCX_SM_RCWT && opt->write_format == CCX_OF_RCWT && opt->output_filename == NULL)
	{
		print_error(opt->gui_mode_reports,
			    "CCExtractor's binary format can only be used simultaneously for input and\noutput if the output file name is specified given with -o.\n");
		return EXIT_INCOMPATIBLE_PARAMETERS;
	}
	if (opt->write_format != CCX_OF_DVDRAW && opt->cc_to_stdout && opt->extract == 12)
	{
		print_error(opt->gui_mode_reports, "You can't extract both fields to stdout at the same time in broadcast mode.\n");
		return EXIT_INCOMPATIBLE_PARAMETERS;
	}
	if (opt->write_format == CCX_OF_SPUPNG && opt->cc_to_stdout)
	{
		print_error(opt->gui_mode_reports, "You cannot use --out=spupng with -stdout.\n");
		return EXIT_INCOMPATIBLE_PARAMETERS;
	}

	if (opt->write_format == CCX_OF_WEBVTT && opt->enc_cfg.encoding != CCX_ENC_UTF_8)
	{
		mprint("Note: Output format is WebVTT, forcing UTF-8\n");
		opt->enc_cfg.encoding = CCX_ENC_UTF_8;
	}
#ifdef WITH_LIBCURL
	if (opt->write_format == CCX_OF_CURL && opt->curlposturl == NULL)
	{
		print_error(opt->gui_mode_reports, "You must pass a URL (-curlposturl) if output format is curl\n");
		return EXIT_INCOMPATIBLE_PARAMETERS;
	}
	if (opt->write_format != CCX_OF_CURL && opt->curlposturl != NULL)
	{
		print_error(opt->gui_mode_reports, "-curlposturl requires that the format is curl\n");
		return EXIT_INCOMPATIBLE_PARAMETERS;
	}
#endif // WITH_LIBCURL
	/* Initialize some Encoder Configuration */
	opt->enc_cfg.extract = opt->extract;
	if (opt->num_input_files > 0)
	{
		opt->enc_cfg.multiple_files = 1;
		opt->enc_cfg.first_input_file = opt->inputfile[0];
	}
	opt->enc_cfg.cc_to_stdout = opt->cc_to_stdout;
	opt->enc_cfg.write_format = opt->write_format;
	opt->enc_cfg.send_to_srv = opt->send_to_srv;
	opt->enc_cfg.date_format = opt->date_format;
	opt->enc_cfg.transcript_settings = opt->transcript_settings;
	opt->enc_cfg.millis_separator = opt->millis_separator;
	opt->enc_cfg.no_font_color = opt->nofontcolor;
	opt->enc_cfg.force_flush = opt->force_flush;
	opt->enc_cfg.append_mode = opt->append_mode;
	opt->enc_cfg.ucla = opt->ucla;
	opt->enc_cfg.no_type_setting = opt->notypesetting;
	opt->enc_cfg.subs_delay = opt->subs_delay;
	opt->enc_cfg.gui_mode_reports = opt->gui_mode_reports;
	if (opt->enc_cfg.render_font == NULL)
	{
		opt->enc_cfg.render_font = DEFAULT_FONT_PATH;
	}
	if (opt->enc_cfg.render_font_italics == NULL)
	{
		opt->enc_cfg.render_font_italics = DEFAULT_FONT_PATH_ITALICS;
	}
	if (opt->output_filename && opt->multiprogram == CCX_FALSE)
	{
		opt->enc_cfg.output_filename = strdup(opt->output_filename);
	}
	else
	{
		opt->enc_cfg.output_filename = NULL;
	}

	if (!opt->is_608_enabled && !opt->is_708_enabled)
	{
		// If nothing is selected then extract both 608 and 708 subs

		// 608 field 1 is enabled by default
		// Enable 708 subs extraction
		parse_708_services(opt, "all");
	}
	else if (!opt->is_608_enabled && opt->is_708_enabled)
	{
		// Extract only 708 subs

		// 608 field 1 is enabled by default, disable it
		opt->extract = 0;
		opt->enc_cfg.extract_only_708 = 1;
	}

#ifdef WITH_LIBCURL
	opt->enc_cfg.curlposturl = opt->curlposturl;
#endif
	return EXIT_OK;
}
