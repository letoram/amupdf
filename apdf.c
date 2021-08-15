#include <arcan_shmif.h>
#include "mupdf/fitz.h"

#define MIN_(a, b)	((a) < (b) ? (a) : (b))

static struct {
	struct arcan_shmif_cont con;
	struct arcan_shmif_initial dpy;
	size_t hw, hh;

	bool dynamic;

	fz_context* ctx;
	fz_document* doc;
	fz_pixmap* pmap;
	fz_device *dev;
	fz_page *page;

	int page_no;
} apdf = {
	.dynamic = true
};

static fz_matrix get_transform(int scale_pct)
{
	fz_matrix ctm;
	ctm = fz_scale((float) scale_pct / 100, (float) scale_pct / 100);
	ctm = fz_pre_rotate(ctm, 0);
	return ctm;
}

static void render()
{
	if (!apdf.dev)
		return;

/* clear to white so blending works */
	for (size_t y = 0; y < apdf.con.h; y++){
		shmif_pixel* out = &apdf.con.vidp[y * apdf.con.pitch];
		for (size_t x = 0; x < apdf.con.w; x++){
			out[x] = SHMIF_RGBA(0xff, 0xff, 0xff, 0xff);
		}
	}

	bool repack = true;
	fz_try(apdf.ctx)
		fz_run_page_contents(apdf.ctx, apdf.page, apdf.dev, get_transform(100), NULL);

/* option here: toggle annotations on / off - possibly also as an overlay
 * window so the WM gets to split out */

	fz_catch(apdf.ctx){
		fprintf(stderr, "couldn't run page: %s\n", fz_caught_message(apdf.ctx));
		repack = false;
	}

	if (!repack)
		return;

	arcan_shmif_signal(&apdf.con, SHMIF_SIGVID);
}

static void set_page(int no)
{
	if (apdf.page)
		apdf.page = (fz_drop_page(apdf.ctx, apdf.page), NULL);

	int np = fz_count_pages(apdf.ctx, apdf.doc);
	if (np)
		no = no % np;

	fz_try(apdf.ctx)
		apdf.page = fz_load_page(apdf.ctx, apdf.doc, no);
	fz_catch(apdf.ctx){
		fprintf(stderr, "couldn't load page %d: %s\n", no, fz_caught_message(apdf.ctx));
		return;
	}

	apdf.page_no = no;
}

static void rebuild_pixmap()
{
	if (apdf.pmap){
		apdf.pmap = (fz_drop_pixmap(apdf.ctx, apdf.pmap), NULL);
		apdf.dev = (fz_drop_device(apdf.ctx, apdf.dev), NULL);
	}

/* two different modes here, one is where we switch page size to fit when stepping
 * (assuming it has changed from last time) - or we pan. There is also:
 * fz_is_document_reflowable -> fz_layout_document(ctx, doc, w, h, em_font_sz) */
	if (apdf.dynamic){
		fz_rect rect = fz_bound_page(apdf.ctx, apdf.page);
		fz_transform_rect(rect, get_transform(100));
		fz_irect bbox = fz_round_rect(rect);
		arcan_shmif_resize(&apdf.con, bbox.x1 - bbox.x0, bbox.y1 - bbox.y0);
	}
	else {
		arcan_shmif_resize(&apdf.con, apdf.hw, apdf.hh);
	}

/* Why the actual f- do they permit a transformation matrix that doesn't have
 * translation and instead want you to recreate the pixmap if you try to pan?!
 * the design is so backwards. Similarly, there is no decent distinction
 * between the colorspace and the packing, so you can get RGB or BGR but not
 * pack with x in alpha. Manually setting 'n' will still not get the right
 * layout so we still get to repack. The icing on this already moldy cake is
 * their own jmp_buf error handling because of course they do.
 *
 * Other options:
 *  - decipher the colorspace structure to add our own repacking (?)
 *  - vendoring and just patching (huge lib though..)
 *
 */
	fz_colorspace* cspace = fz_device_rgb(apdf.ctx);
	fz_try(apdf.ctx)
		apdf.pmap = fz_new_pixmap_with_data(apdf.ctx, cspace,
			apdf.con.w, apdf.con.h, NULL, 1, apdf.con.stride, apdf.con.vidb);

	fz_catch(apdf.ctx){
		fprintf(stderr, "pixmap creation failed: %s\n", fz_caught_message(apdf.ctx));
	}

/*
 * so we actually need to calculate the bounding box and then clip to our
 * current context size and pan there
 */
	apdf.pmap->flags &= ~1; /* stop it from free:ing our buffer */
	float dpi = apdf.dpy.density * 2.54;
	fz_set_pixmap_resolution(apdf.ctx, apdf.pmap, dpi, dpi);

	fz_try(apdf.ctx)
		apdf.dev = fz_new_draw_device(apdf.ctx, get_transform(100), apdf.pmap);

	fz_catch(apdf.ctx){
		fprintf(stderr, "device creation failed: %s\n", fz_caught_message(apdf.ctx));
	}

/* another interesting bit here is that we could add our own font hooks and map
 * that to the fonthints that we receive over the connection */
}

static bool process_input(arcan_ioevent* ev)
{
	if (ev->kind == EVENT_IO_BUTTON && ev->input.digital.active){
		set_page(apdf.page_no + 1);
		return true;
	}

/* expose labels for input (LASTPAGE, NEXTPAGE, PREVPAGE, NEXTCH, ...)
 * bookmark -> fz_make_bookmark(ctx, doc, loc) */

	return false;
}

static bool run_event(struct arcan_event* ev)
{
	if (ev->category == EVENT_IO)
		return process_input(&ev->io);

/* an interesting possiblity when this is added to afsrv_decode is to have an
 * AGP backend that translates into PDF -> renders through decode and then just
 * maps the buffer to the KMS plane */

/* tons of PDF features to map here, selection of text, stepping blocks,
 * converting to accessibility segment, converting to clipboard segment,
 * outline, password authentication, form, bookmarks, links, annotations,
 * forwarding the content appropriate side or force scaling, decryption key,
 * ... */

	if (ev->category != EVENT_TARGET)
		return false;

	if (ev->tgt.kind == TARGET_COMMAND_DISPLAYHINT){
		bool rv = false;
		if (ev->tgt.ioevs[0].iv && ev->tgt.ioevs[1].iv){
			apdf.hw = ev->tgt.ioevs[0].iv;
			apdf.hh = ev->tgt.ioevs[1].iv;
			rv = true;
		}
		if (ev->tgt.ioevs[4].fv && ev->tgt.ioevs[4].fv != apdf.dpy.density){
			apdf.dpy.density = ev->tgt.ioevs[4].fv;
			rv = true;
		}
		return rv;
	}

	if (ev->tgt.kind == TARGET_COMMAND_SEEKCONTENT){
/* figure out page and possibly bbox offset if we don't fit */
	}
	else if (ev->tgt.kind == TARGET_COMMAND_BCHUNK_IN){
/* if seekable, just wrap dup:ed FD in file, otherwise read into buffer and
 * wrap memory buffer as FILE */
	}

/* for STATE_IN/OUT we need some magic signature, bake in the contents of the
 * document itself, grab bookmarks and annotations */

/* fz_count_pages -> get range for contenthint */
	return false;
}

int main(int argc, char** argv)
{
	if (argc <= 1){
		fprintf(stdout, "Missing file argument\n");
		return EXIT_FAILURE;
	}

	apdf.ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	fz_register_document_handlers(apdf.ctx);

	fz_try(apdf.ctx) {
		apdf.doc = fz_open_document(apdf.ctx, argv[1]);
	}
	fz_catch(apdf.ctx) {
		fprintf(stdout, "Couldn't parse/read file\n");
		fz_drop_context(apdf.ctx);
		return EXIT_FAILURE;
	}

	apdf.con = arcan_shmif_open(SEGID_MEDIA, SHMIF_ACQUIRE_FATALFAIL, 0);

/* keep this around so we can just re-use for DISPLAYHINT changes */
	struct arcan_shmif_initial* init;
	arcan_shmif_initial(&apdf.con, &init);
	apdf.dpy = *init;
	apdf.hw = apdf.con.w;
	apdf.hh = apdf.con.h;

	set_page(0);
	rebuild_pixmap();
	render();

	struct arcan_event ev;
	bool dirty = false;

/* normal double-dispatch structure to deal with storms without unnecessary refreshes */
	while (arcan_shmif_wait(&apdf.con, &ev)){
		dirty |= run_event(&ev);
		while (arcan_shmif_poll(&apdf.con, &ev) > 0){
			dirty |= run_event(&ev);
		}
		if (dirty){
			render();
			dirty = false;
		}
	}

	fz_drop_document(apdf.ctx, apdf.doc);
	fz_drop_context(apdf.ctx);
	arcan_shmif_drop(&apdf.con);
	return EXIT_SUCCESS;
}
