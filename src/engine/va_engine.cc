#include "va_engine.h"

#include <cstring>
#include <memory>
#include <tuple>

#include "va_object_meta.h"
#include "va_user_data.h"

static gchar pgie_classes[4][32] = { "Vehicle", "TwoWheeler", "Person", "RoadSign" };

static gboolean PERF_MODE = FALSE;

/**
 * Check if running using config file or .h264 stream file
 */
static auto is_using_config_file(char* argv) -> bool {
	return (g_str_has_suffix(argv, ".yml") || g_str_has_suffix(argv, ".yaml")) ? true : false;
}

/** 
 * Extract metadata, NvDsBatchMeta -> NvDsFrameMeta -> (....) 
 */
static auto tiler_src_pad_buffer_probe(GstPad* pad, GstPadProbeInfo* info, void* user_data) -> GstPadProbeReturn {
	guint num_rects = 0; 
	guint vehicle_count = 0;
	guint person_count = 0;
	NvDsMetaList* l_frame = nullptr;
	NvDsMetaList* l_obj = nullptr;
	NvDsObjectMeta* object_meta = nullptr;
	// NvDsDisplayMeta* display_meta = nullptr;
	NvDsFrameMeta* frame_meta = nullptr;

	va::UserData* va_user_data = static_cast<va::UserData*>(user_data);
	va::Database* va_database = static_cast<va::Database*>(va_user_data->va_database);

	GstBuffer* buf = static_cast<GstBuffer*>(info->data);
	NvDsBatchMeta* batch_meta = gst_buffer_get_nvds_batch_meta(buf);
	for (l_frame = batch_meta->frame_meta_list; l_frame != nullptr; l_frame = l_frame->next) {
		frame_meta = static_cast<NvDsFrameMeta*>(l_frame->data);
		
		std::string video_file = { "test" };
		va::FrameMetadata va_frame_meta {
			video_file,
			frame_meta->ntp_timestamp // frame timestamp
		};
		for (l_obj = frame_meta->obj_meta_list; l_obj != nullptr; l_obj = l_obj->next) {
			object_meta = static_cast<NvDsObjectMeta*>(l_obj->data);

			if (va_database) {
				va_frame_meta.va_object_meta_list.emplace_back(
					object_meta, 
					pgie_classes[object_meta->class_id], 
					frame_meta->ntp_timestamp
				);
				// std::cout << va_frame_meta.va_object_meta_list.size() << std::endl;
			}

			if (object_meta->class_id == PGIE_CLASS_ID_VEHICLE) {
				++vehicle_count;
				++num_rects;
			}
			if (object_meta->class_id == PGIE_CLASS_ID_PERSON) {
				++vehicle_count;
				++num_rects;
			}
		}

		/* count the frame and reset the count to 0 if it reachs save_interval */
		++va_user_data->frame_count;
		if (va_user_data->frame_count == va_user_data->save_interval) {
			if (va_database) {
				va_database->insert(&va_frame_meta);
			}
			// std::cout << "save at: " << va_user_data->frame_count << std::endl;
			va_user_data->frame_count = 0;
		}

		/* g_print(
			"Frame Number = %d Number of objects = %d " "Vehicle Count = %d Person Count = %d\n",
			frame_meta->frame_num,
			num_rects,
			vehicle_count,
			person_count
		); */

#if 0
		int offset = 0;
		display_meta = nvds_acquire_display_meta_from_pool(batch_meta);

		NvOSD_TextParams* txt_params = &display_meta->text_params[0];
		txt_params->display_text = static_cast<char*>(g_malloc0(MAX_DISPLAY_LEN)); // g_malloc0 return void*, cast from void* to char*
		offset = snprintf(txt_params->display_text, MAX_DISPLAY_LEN, "Person = %d ", person_count);
		offset = snprintf(txt_params->display_text + offset , MAX_DISPLAY_LEN, "Vehicle = %d ", vehicle_count);
		
		/* Now set the offsets where the string should appear */
		txt_params->x_offset = 10;
		txt_params->y_offset = 12;

		/* Font , font-color and font-size */
		txt_params->font_params.font_name = "Serif";
		txt_params->font_params.font_size = 10;
		txt_params->font_params.font_color.red = 1.0;
		txt_params->font_params.font_color.green = 1.0;
		txt_params->font_params.font_color.blue = 1.0;
		txt_params->font_params.font_color.alpha = 1.0;

		/* Text background color */
		txt_params->set_bg_clr = 1;
		txt_params->text_bg_clr.red = 0.0;
		txt_params->text_bg_clr.green = 0.0;
		txt_params->text_bg_clr.blue = 0.0;
		txt_params->text_bg_clr.alpha = 1.0;

		nvds_add_display_meta_to_frame(frame_meta, display_meta);
#endif
	}

	return GST_PAD_PROBE_OK;
}

static auto bus_call(GstBus* bus, GstMessage* msg, gpointer data) -> gboolean {
	GMainLoop* loop = (GMainLoop*)data;
	switch (GST_MESSAGE_TYPE(msg)) {
		case GST_MESSAGE_EOS:
			g_print("End of stream\n");
			g_main_loop_quit(loop);
			break;
		case GST_MESSAGE_WARNING: {
			gchar* debug;
			GError* error;
			gst_message_parse_warning(msg, &error, &debug);
			g_printerr("WARNING from element %s: %s\n", GST_OBJECT_NAME(msg->src), error->message);
			g_free(debug);
			g_printerr("Warning: %s\n", error->message);
			g_error_free(error);
			break;
		}
		case GST_MESSAGE_ERROR: {
			gchar* debug;
			GError* error;
			gst_message_parse_error(msg, &error, &debug);
			g_printerr("ERROR from element %s: %s\n", GST_OBJECT_NAME(msg->src), error->message);
			if (debug) {
				g_printerr("Error details: %s\n", debug);
			}
			g_free(debug);
			g_error_free(error);
			g_main_loop_quit(loop);
			break;
		}
		case GST_MESSAGE_ELEMENT: {
			if (gst_nvmessage_is_stream_eos(msg)) {
				guint stream_id;
				if (gst_nvmessage_parse_stream_eos(msg, &stream_id)) {
					g_print("Got EOS from stream %d\n", stream_id);
				}
			}
			break;
		}
		default:
			break;
	}
	return TRUE;
}

static auto cb_pad_added(GstElement* decodebin, GstPad* decoder_src_pad, gpointer data) -> void {
	GstCaps* caps = gst_pad_get_current_caps(decoder_src_pad);
	if (!caps) {
		caps = gst_pad_query_caps(decoder_src_pad, NULL);
	}
	const GstStructure *str = gst_caps_get_structure(caps, 0);
	const gchar *name = gst_structure_get_name(str);
	GstElement *source_bin = (GstElement*)data;
	GstCapsFeatures *features = gst_caps_get_features(caps, 0);

	/* Need to check if the pad created by the decodebin is for video and not
	 * audio. */
	if (!strncmp(name, "video", 5)) {
		/* Link the decodebin pad only if decodebin has picked nvidia
		 * decoder plugin nvdec_*. We do this by checking if the pad caps contain
		 * NVMM memory features. */
		if (gst_caps_features_contains(features, GST_CAPS_FEATURES_NVMM)) {
			/* Get the source bin ghost pad */
			GstPad *bin_ghost_pad = gst_element_get_static_pad(source_bin, "src");
			if (!gst_ghost_pad_set_target(GST_GHOST_PAD(bin_ghost_pad), decoder_src_pad)) {
				g_printerr("Failed to link decoder src pad to source bin ghost pad\n");
			}
			gst_object_unref(bin_ghost_pad);
		} else {
			g_printerr("Error: Decodebin did not pick nvidia decoder plugin.\n");
		}
	}
}

static auto cb_decodebin_child_added(GstChildProxy* child_proxy, GObject* object, gchar* name, gpointer user_data) -> void {
	g_print("Decodebin child added: %s\n", name);
	if (g_strrstr(name, "decodebin") == name) {
		g_signal_connect(G_OBJECT(object), "child-added", G_CALLBACK(cb_decodebin_child_added), user_data);
	}
	if (g_strrstr(name, "source") == name) {
		g_object_set(G_OBJECT(object), "drop-on-latency", true, NULL);
	}
}

inline auto va::Engine::m_create_pipeline() -> GstElement* {
	GstElement* pipeline = gst_pipeline_new("pipeline");
	if (!pipeline) {
		throw std::runtime_error("Pipeline element could not be created. Exiting.\n");
	}
	g_print("Pipeline created\n");
	return pipeline;
}

inline auto va::Engine::m_create_streamux() -> GstElement* {
	GstElement* streammux = gst_element_factory_make("nvstreammux", "stream-muxer");
	if (!streammux) {
		throw std::runtime_error("Streamux element could not be created. Exiting.\n");
	}
	g_print("Streamux created\n");
	if (!gst_bin_add(GST_BIN(m_pipeline), streammux)) {
		throw std::runtime_error("Streamux element could not be added to pipeline. Exiting.\n");
	}
	return streammux;
}

inline auto va::Engine::m_create_source_bin(guint index, gchar* uri) -> GstElement* {
	GstElement *bin = nullptr, *uri_decode_bin = nullptr;
	gchar bin_name[16] = { };

	g_snprintf(bin_name, 15, "source-bin-%02d", index);
	/* Create a source GstBin to abstract this bin's content from the rest of the
	 * pipeline */
	bin = gst_bin_new(bin_name);

	/* Source element for reading from the uri.
	 * We will use decodebin and let it figure out the container format of the
	 * stream and the codec and plug the appropriate demux and decode plugins. */
	if (PERF_MODE) {
		uri_decode_bin = gst_element_factory_make("nvurisrcbin", "uri-decode-bin");
		g_object_set(G_OBJECT(uri_decode_bin), "file-loop", TRUE, NULL);
	} else {
		uri_decode_bin = gst_element_factory_make("uridecodebin", "uri-decode-bin");
	}

	if (!bin || !uri_decode_bin) {
		throw std::runtime_error("One element in source bin could not be created.\n");
	}

	/* We set the input uri to the source element */
	g_object_set(G_OBJECT(uri_decode_bin), "uri", uri, NULL);

	/* Connect to the "pad-added" signal of the decodebin which generates a
	 * callback once a new pad for raw data has beed created by the decodebin */
	g_signal_connect(G_OBJECT(uri_decode_bin), "pad-added", G_CALLBACK(cb_pad_added), bin);
	g_signal_connect(G_OBJECT(uri_decode_bin), "child-added", G_CALLBACK(cb_decodebin_child_added), bin);
	gst_bin_add(GST_BIN(bin), uri_decode_bin);

	/* We need to create a ghost pad for the source bin which will act as a proxy
	 * for the video decoder src pad. The ghost pad will not have a target right
	 * now. Once the decode bin creates the video decoder and generates the
	 * cb_pad_added callback, we will set the ghost pad target to the video decoder
	 * src pad. */
	if (!gst_element_add_pad(bin, gst_ghost_pad_new_no_target("src", GST_PAD_SRC))) {
		throw std::runtime_error("Failed to add ghost pad in source bin\n");
	}

	return bin;
}

inline auto va::Engine::m_add_source_bin_to_pipeline() -> void {
	GList* src_list = nullptr;

	if (is_using_config_file(m_argv[1])) {
		nvds_parse_source_list(&src_list, m_argv[1], "source-list");
		GList* temp_src_list = src_list;
		while (temp_src_list) {
			++m_num_sources;
			temp_src_list = temp_src_list->next;
		}
		g_list_free(temp_src_list);
	} else {
		m_num_sources = m_argc - 1;
	}

	for (guint i = 0; i < m_num_sources; ++i) {
		GstPad *sinkpad, *srcpad;
		gchar pad_name[16] = { };

		GstElement* source_bin = nullptr;
		if (is_using_config_file(m_argv[1])) {
			g_print("Now playing : %s\n", (char*)(src_list)->data);
			source_bin = m_create_source_bin(i, (char*)(src_list)->data);
		} else {
			source_bin = m_create_source_bin(i, m_argv[i + 1]);
		}
		if (!source_bin) {
			throw std::runtime_error("Failed to create source bin. Exiting.\n");
		}

		if (!gst_bin_add(GST_BIN(m_pipeline), source_bin)) {
			throw std::runtime_error("Failed to add source bin to pipeline. Exiting.\n");
		}

		g_snprintf(pad_name, 15, "sink_%u", i);
		sinkpad = gst_element_get_request_pad(m_streammux, pad_name);
		if (!sinkpad) {
			throw std::runtime_error("Streammux request sink pad failed. Exiting.\n");
		}

		srcpad = gst_element_get_static_pad(source_bin, "src");
		if (!srcpad) {
			throw std::runtime_error("Failed to get src pad of source bin. Exiting.\n");
		}

		if (gst_pad_link(srcpad, sinkpad) != GST_PAD_LINK_OK) {
			throw std::runtime_error("Failed to link source bin to stream muxer. Exiting.\n");
		}

		gst_object_unref(srcpad);
		gst_object_unref(sinkpad);

		if (is_using_config_file(m_argv[1])) {
			src_list = src_list->next;
		}
	}

	if (is_using_config_file(m_argv[1])) {
		g_list_free(src_list);
	}
}

inline auto va::Engine::m_create_nvinfer() -> GstElement* {
	GstElement* infer = gst_element_factory_make("nvinfer", "primary-nvinference-engine");
	if (!infer) {
		throw std::runtime_error("NvInfer element could not be created. Exiting.\n");
	}
	g_print("NvInfer created\n");
	return infer;
}

inline auto va::Engine::m_create_queue() -> std::tuple<GstElement*, GstElement*, GstElement*, GstElement*, GstElement*> {
	GstElement* queue1 = gst_element_factory_make("queue", "queue1");
	GstElement* queue2 = gst_element_factory_make("queue", "queue2");
	GstElement* queue3 = gst_element_factory_make("queue", "queue3");
	GstElement* queue4 = gst_element_factory_make("queue", "queue4");
	GstElement* queue5 = gst_element_factory_make("queue", "queue5");
	return std::make_tuple(queue1, queue2, queue3, queue4, queue5);
}

inline auto va::Engine::m_create_nvdslogger() -> GstElement* {
	GstElement* nvdslogger = gst_element_factory_make ("nvdslogger", "nvdslogger");
	if (!nvdslogger) {
		throw std::runtime_error("NvDsLogger element could not be created. Exiting.\n");
	}
	g_print("NvDsLogger created\n");
	return nvdslogger;
}

inline auto va::Engine::m_create_tiler() -> GstElement* {
	GstElement* tiler = gst_element_factory_make("nvmultistreamtiler", "nvtiler");
	if (!tiler) {
		throw std::runtime_error("Tiler element could not be created. Exiting.\n");
	}
	g_print("Tiler created\n");
	return tiler;
}

inline auto va::Engine::m_create_nvvidconv() -> GstElement* {
	GstElement* nvvidconv = gst_element_factory_make("nvvideoconvert", "nvvideo-converter");
	if (!nvvidconv) {
		throw std::runtime_error("NvVidConv element could not be created. Exiting.\n");
	}
	g_print("NvVidConv created\n");
	return nvvidconv;
}

inline auto va::Engine::m_create_nvvidosd() -> GstElement* {
	GstElement* nvosd = gst_element_factory_make("nvdsosd", "nv-onscreendisplay");
	if (!nvosd) {
		throw std::runtime_error("NvOsd element could not be created. Exiting.\n");
	}
	g_print("NvOsd created\n");
	return nvosd;
}

inline auto va::Engine::m_create_transform() -> GstElement* {
	if (!m_cuda_prop.integrated) {
		return nullptr;
	}
	GstElement* transform = gst_element_factory_make("nvegltransform", "nvegl-transform");
	if (!transform) {
		throw std::runtime_error("One Tegra element could not be created. Exiting.\n");
	}
	g_print("NvEglTransform created\n");
	return transform;
}

inline auto va::Engine::m_create_sink() -> GstElement* {
	GstElement* sink = nullptr;
	/* Profiling mode */
	if (PERF_MODE) {
		sink = gst_element_factory_make("fakesink", "nvvideo-renderer");
	} else {
		/* Finally render the osd output */
		// m_create_transform();
		sink = gst_element_factory_make("nveglglessink", "nvvideo-renderer");
	}
	if (!sink) {
		throw std::runtime_error("Sink element could not be created. Exiting.\n");
	}
	g_print("Sink created\n");
	return sink;
}

inline auto va::Engine::m_setup_element_config() -> void {
	m_tiler_rows = (guint)sqrt(m_num_sources);
	m_tiler_columns = (guint)ceil(1.0 * m_num_sources / m_tiler_rows);
	if (is_using_config_file(m_argv[1])) {
		nvds_parse_streammux(m_streammux, m_argv[1], "streammux");
		g_object_set(G_OBJECT(m_nvinfer), "config-file-path", "configs/pgie_config.yml", NULL);
		g_object_get(G_OBJECT(m_nvinfer), "batch-size", &m_nvinfer_batch_size, NULL);
		if (m_nvinfer_batch_size != m_num_sources) {
			g_printerr("WARNING: Overriding infer-config batch-size (%d) with number of sources (%d)\n", m_nvinfer_batch_size, m_num_sources);
			g_object_set(G_OBJECT(m_nvinfer), "batch-size", m_num_sources, NULL);
		}

		nvds_parse_osd(m_nvosd, m_argv[1], "osd");
		g_object_set(G_OBJECT(m_tiler), "rows", m_tiler_rows, "columns", m_tiler_columns, NULL);

		nvds_parse_tiler(m_tiler, m_argv[1], "tiler");
		nvds_parse_egl_sink(m_sink, m_argv[1], "sink");
	} else {
		g_object_set(G_OBJECT(m_streammux), "batch-size", m_num_sources, NULL);
		g_object_set(
			G_OBJECT(m_streammux),
			"width",
			MUXER_OUTPUT_WIDTH,
			"height",
			MUXER_OUTPUT_HEIGHT,
			"batched-push-timeout",
			MUXER_BATCH_TIMEOUT_USEC,
			NULL
		);

		/* Configure the nvinfer element using the nvinfer config file. */
		g_object_set(G_OBJECT(m_nvinfer), "config-file-path", "configs/pgie_config.txt", NULL);

		/* Override the batch-size set in the config file with the number of sources. */
		g_object_get (G_OBJECT(m_nvinfer), "batch-size", &m_nvinfer_batch_size, NULL);
		if (m_nvinfer_batch_size != m_num_sources) {
			g_printerr("WARNING: Overriding infer-config batch-size (%d) with number of sources (%d)\n", m_nvinfer_batch_size, m_num_sources);
			g_object_set(G_OBJECT(m_nvinfer), "batch-size", m_num_sources, NULL);
		}

		/* we set the tiler properties here */
		g_object_set(
			G_OBJECT(m_tiler),
			"rows",
			m_tiler_rows,
			"columns",
			m_tiler_columns,
			"width",
			TILED_OUTPUT_WIDTH,
			"height",
			TILED_OUTPUT_HEIGHT,
			NULL
		);

		g_object_set(G_OBJECT(m_nvosd), "process-mode", OSD_PROCESS_MODE, "display-text", OSD_DISPLAY_TEXT, NULL);
		g_object_set(G_OBJECT(m_sink), "qos", 0, NULL);
	}
}

inline auto va::Engine::m_add_elements_to_pipeline() -> void {
	if (m_transform) {
		gst_bin_add_many(GST_BIN(m_pipeline), m_queue1, m_nvinfer, m_queue2, m_nvdslogger, m_tiler,
				m_queue3, m_nvvidconv, m_queue4, m_nvosd, m_queue5, m_transform, m_sink, NULL);
		/* we link the elements together
		 * nvstreammux -> nvinfer -> nvdslogger -> nvtiler -> nvvidconv -> nvosd
		 * -> video-renderer */
		if (!gst_element_link_many(m_streammux, m_queue1, m_nvinfer, m_queue2, m_nvdslogger, m_tiler,
					m_queue3, m_nvvidconv, m_queue4, m_nvosd, m_queue5, m_transform, m_sink, NULL)) {
			throw std::runtime_error("Elements could not be linked. Exiting.\n");
		}
	} else {
		gst_bin_add_many(GST_BIN(m_pipeline), m_queue1, m_nvinfer, m_queue2, m_nvdslogger, m_tiler,
				m_queue3, m_nvvidconv, m_queue4, m_nvosd, m_queue5, m_sink, NULL);
		// gst_bin_add_many(GST_BIN(m_pipeline), m_nvinfer, m_nvdslogger, m_tiler, m_nvvidconv, m_sink, NULL);
		/* we link the elements together
		 * nvstreammux -> nvinfer -> nvdslogger -> nvtiler -> nvvidconv -> nvosd
		 * -> video-renderer */
		if (!gst_element_link_many(m_streammux, m_queue1, m_nvinfer, m_queue2, m_nvdslogger, m_tiler,
					m_queue3, m_nvvidconv, m_queue4, m_nvosd, m_queue5, m_sink, NULL)) {
			throw std::runtime_error("Elements could not be linked. Exiting.\n");
		}
		/* if (!gst_element_link_many(m_streammux, m_nvinfer, m_nvdslogger, m_tiler, m_nvvidconv, m_sink, NULL)) {
			throw std::runtime_error("Elements could not be linked. Exiting.\n");
		} */
	}
}

inline auto va::Engine::m_create_message_handler() -> guint {
	GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
	guint bus_watch_id = gst_bus_add_watch(bus, bus_call, m_loop);
	gst_object_unref(bus);
	return bus_watch_id;
}

inline auto va::Engine::m_add_tiler_src_pad_buffer_probe(va::UserData* va_user_data) -> void {
	GstPad* tiler_src_pad = gst_element_get_static_pad(m_nvinfer, "src");
	if (!tiler_src_pad) {
		throw std::runtime_error("Unable to get NvInfer src pad\n");
	} else {
		gst_pad_add_probe(tiler_src_pad, GST_PAD_PROBE_TYPE_BUFFER, tiler_src_pad_buffer_probe, (void*)va_user_data, NULL);
	}
	gst_object_unref(tiler_src_pad);
}

auto va::Engine::run() -> void {
	int save_interval = 60;
	va::UserData va_user_data { m_va_database, save_interval };

	/* Standard GStreamer initialization */
	gst_init(&m_argc, &m_argv);
	m_loop = g_main_loop_new(NULL, FALSE);

	/* Create gstreamer elements */
	/* Create Pipeline element that will form a connection of other elements */
	m_pipeline = m_create_pipeline();
	/* Create nvstreammux instance to form batches from one or more sources. */
	m_streammux = m_create_streamux();
	/* Create a list of sources bin and add it to pipeline for batching input. */
	m_add_source_bin_to_pipeline();
	/* Use nvinfer to infer on batched frame. */
	m_nvinfer = m_create_nvinfer();
	/* Create queue elements to add between every two elements */
	std::tie(m_queue1, m_queue2, m_queue3, m_queue4, m_queue5) = m_create_queue();
	/* Use nvdslogger for perf measurement. */
	m_nvdslogger = m_create_nvdslogger();
	/* Use nvtiler to composite the batched frames into a 2D tiled array based on the source of the frames. */
	m_tiler = m_create_tiler();
	/* Use convertor to convert from NV12 to RGBA as required by nvosd */
	m_nvvidconv = m_create_nvvidconv();
	/* Create OSD to draw on the converted RGBA buffer */
	m_nvosd = m_create_nvvidosd();
	/* Create transform */
	m_transform = m_create_transform();
	/* Create sink */
	m_sink = m_create_sink();
	/* Load config for elements after creating them */
	m_setup_element_config();
	/* we add a message handler */
	m_bus_watch_id = m_create_message_handler();

	/* Set up the pipeline */
	/* we add all elements into the pipeline */
	m_add_elements_to_pipeline();

	/* Lets add probe to get informed of the meta data generated, we add probe to
	 * the sink pad of the osd element, since by that time, the buffer would have
	 * had got all the metadata. */
	m_add_tiler_src_pad_buffer_probe(&va_user_data);

	/* Set the pipeline to "playing" state */
	if (is_using_config_file(m_argv[1])) {
		g_print("Using file: %s\n", m_argv[1]);
	} else {
		g_print("Now playing:");
		for (guint i = 0; i < m_num_sources; ++i) {
			g_print(" %s,", m_argv[i + 1]);
		}
		g_print("\n");
	}
	PERF_MODE = g_getenv("PERF_MODE") && !g_strcmp0(g_getenv("PERF_MODE"), "1");

	/* Start playing */
	gst_element_set_state(m_pipeline, GST_STATE_PLAYING);

	/* Wait till pipeline encounters an error or EOS */
	g_print("Running...\n");
	g_main_loop_run(m_loop);

	/* Out of the main loop, clean up nicely */
	g_print("Returned, stopping playback\n");
	gst_element_set_state(m_pipeline, GST_STATE_NULL);
	g_print("Deleting pipeline\n");
	gst_object_unref(GST_OBJECT(m_pipeline));
	g_source_remove(m_bus_watch_id);
	g_main_loop_unref(m_loop);
}

auto va::Engine::set_database(va::Database* _va_database) -> void {
	m_va_database = _va_database;
}

va::Engine::Engine(int argc, char** argv) : m_argc(argc), m_argv(argv) {
	/* Check input arguments */
	if (m_argc < 2) {
		g_printerr("Usage: %s <yml file>\n", m_argv[0]);
		g_printerr("OR: %s <uri1> [uri2] ... [uriN] \n", m_argv[0]);
		throw std::invalid_argument("invalid argument\n");
	}

	int current_device = -1; // automatically find CUDA device
	cudaGetDevice(&current_device);
	cudaGetDeviceProperties(&m_cuda_prop, current_device);
	g_print("current CUDA device: %d\n", current_device);
}

va::Engine::~Engine() {
	std::cout << "start VA ENGINE deallocate" << std::endl;
	std::cout << "finish VA ENGINE deallocate" << std::endl;
}
