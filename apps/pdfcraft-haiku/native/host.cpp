/*
 * PdfCraft's air/OS (Haiku) host: the application, its window and the system
 * services the Rust side reaches through the C functions at the end of this
 * file (bound in src/host.rs).
 *
 * Threads: be_app runs its message loop on the process's main thread
 * (pc_host_run), the window on its own looper thread, and the Rust interface
 * on a thread of its own. Input arrives in the window thread and is queued
 * here for the Rust thread, which waits on the queue between frames
 * (pc_host_wait) and hands back finished frames as 32-bit BGRA pixels
 * (pc_host_present), or puts them straight into the screen itself (the window
 * is a direct window; src/present.rs). Every call from the Rust thread that
 * touches the window locks it first.
 */

#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <Clipboard.h>
#include <ControlLook.h>
#include <Cursor.h>
#include <DirectWindow.h>
#include <Directory.h>
#include <Entry.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <Font.h>
#include <FormattingConventions.h>
#include <Input.h>
#include <InterfaceDefs.h>
#include <Key.h>
#include <KeyStore.h>
#include <Locale.h>
#include <LocaleRoster.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Message.h>
#include <Messenger.h>
#include <Mime.h>
#include <Node.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Region.h>
#include <Roster.h>
#include <Screen.h>
#include <String.h>
#include <Url.h>
#include <View.h>
#include <Window.h>

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>


extern "C" {

// Keep in step with `Event` in src/host.rs.
struct PcEvent {
	int32_t kind;
	int32_t buttons;
	int32_t clicks;
	uint32_t modifiers;
	int32_t key;
	int32_t raw_char;
	int32_t repeat;
	int32_t id;
	float x;
	float y;
	float dx;
	float dy;
	uint32_t text_len;
};

struct PcWindowInfo {
	float inner_left, inner_top, inner_right, inner_bottom;
	float outer_left, outer_top, outer_right, outer_bottom;
	float screen_width, screen_height;
	int32_t minimized;
	int32_t active;
	int32_t zoomed;
	int32_t fullscreen;
};

}


enum {
	PC_MOUSE_MOVED = 1,
	PC_MOUSE_DOWN = 2,
	PC_MOUSE_UP = 3,
	PC_WHEEL = 4,
	PC_KEY_DOWN = 5,
	PC_KEY_UP = 6,
	PC_RESIZED = 7,
	PC_FOCUS = 8,
	PC_CLOSE = 9,
	PC_OPEN_FILES = 10,
	PC_DROPPED_FILES = 11,
	PC_POINTER_GONE = 12,
	PC_QUIT_REQUESTED = 13,
	PC_DIALOG_ANSWER = 14,
	PC_MODIFIERS = 15,
	PC_MOVED = 16,
	PC_MINIMIZED = 17,
	PC_COLORS = 18,
	PC_MENU = 19,
	PC_IME = 20,
};

// What a PC_IME event's id says.
enum {
	PC_IME_PREEDIT = 1,
	PC_IME_COMMIT = 2,
	PC_IME_STOPPED = 3,
};

static const uint32 kDialogAnswer = 'dcDA';
static const char* kDialogField = "dc:dialog";
// A menu bar item was chosen: its command ("pc:command", src/menu.rs's key).
static const uint32 kMenuChosen = 'pcMN';
static const char* kMenuCommandField = "pc:command";
// A menu update that waited for the menus to close.
static const uint32 kMenuApply = 'pcMA';
// A file panel's file type menu: show the files of filter "pc:filter".
static const uint32 kFilterChosen = 'pcFL';
static const char* kFilterField = "pc:filter";
static const char* kFilterMenuName = "pc:filters";


// #pragma mark - Event queue


struct QueuedEvent {
	PcEvent event;
	std::string text;
};

static std::mutex sQueueLock;
static std::condition_variable sQueueCondition;
static std::deque<QueuedEvent> sQueue;
static bool sWoken = false;
// The text of the event pc_host_next_event returned last.
static std::string sCurrentText;
static volatile bool sAllowQuit = false;
static bool sFullscreen = false;
static BRect sWindowedFrame;


static PcEvent
MakeEvent(int32_t kind)
{
	PcEvent event;
	memset(&event, 0, sizeof(event));
	event.kind = kind;
	event.modifiers = modifiers();
	return event;
}


static void
Push(const PcEvent& event, const std::string& text = std::string())
{
	{
		std::lock_guard<std::mutex> lock(sQueueLock);
		// A burst of moves only needs the last position.
		if (event.kind == PC_MOUSE_MOVED && !sQueue.empty()
			&& sQueue.back().event.kind == PC_MOUSE_MOVED
			&& sQueue.back().event.buttons == event.buttons) {
			sQueue.back().event = event;
		} else {
			QueuedEvent queued;
			queued.event = event;
			queued.text = text;
			sQueue.push_back(queued);
		}
	}
	sQueueCondition.notify_all();
}


// The paths of a message's refs, one per line. As documents, without the
// application's own executable: a launch through a link to it can hand it its
// own ref, and that is never a PDF.
static std::string
PathsOf(const BMessage* message, bool documents = false)
{
	app_info self;
	bool knowSelf = documents && be_app->GetAppInfo(&self) == B_OK;
	std::string paths;
	entry_ref ref;
	for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
		BEntry entry(&ref, true);
		entry_ref resolved;
		BPath path;
		if (entry.GetRef(&resolved) != B_OK || entry.GetPath(&path) != B_OK)
			continue;
		if (knowSelf && resolved == self.ref)
			continue;
		if (!paths.empty())
			paths += '\n';
		paths += path.Path();
	}
	return paths;
}


// #pragma mark - Input methods


// The input method transaction in progress (B_INPUT_METHOD_STARTED until
// B_INPUT_METHOD_STOPPED): where to answer, the text being composed (an
// answer to a location request has a point per character), and where the
// interface's caret is (canvas view coordinates, logical pixels; from the
// Rust thread, pc_host_ime_caret).
static std::mutex sImeLock;
static BMessenger sImeMethod;
static std::string sImeText;
static BRect sImeCaret;
static bool sImeCaretKnown = false;


static int32
CountUtf8Characters(const std::string& text)
{
	int32 count = 0;
	for (size_t i = 0; i < text.size(); i++) {
		if (((uint8)text[i] & 0xc0) != 0x80)
			count++;
	}
	return count;
}


// #pragma mark - Direct connection


#ifndef B_DIRECT_DEVICE_PIXELS
#define B_DIRECT_DEVICE_PIXELS 0x00000400
#endif

// Keep in step with `RawDirect` in src/host.rs.
typedef struct {
	int32_t connected;
	int32_t bounds[4];
	uint32_t bytesPerRow;
	uint32_t rectCount;
	const int32_t* rects;
} PcDirect;

typedef void (*PcDirectSink)(const PcDirect* direct);

static const uint32 kMaxDirectRects = 256;

// The window's last direct connection and where it is reported to (the
// presenter, src/present.rs). The sink is called with sDirectLock held, so
// unsetting it waits for a report in progress.
static std::mutex sDirectLock;
static PcDirectSink sDirectSink = NULL;
static PcDirect sDirect;
static bool sDirectReported = false;
// Where the canvas starts in the window (logical pixels): under the menu bar, if
// there is one. The window's direct connection covers the menu bar too; frames
// go into the screen only below it.
static std::atomic<float> sCanvasTop(0);
static int32_t sDirectRects[kMaxDirectRects * 4];


// Frames go straight into the screen only when PDFCRAFT_PRESENT=direct asks for
// it (src/present.rs) and the GPU draws them.
static uint32
DirectPresentFlags()
{
	const char* present = getenv("PDFCRAFT_PRESENT");
	const char* renderer = getenv("PDFCRAFT_RENDERER");
	if (present == NULL || strcmp(present, "direct") != 0
		|| (renderer != NULL && strcmp(renderer, "cpu") == 0))
		return 0;
	return B_DIRECT_DEVICE_PIXELS;
}


// #pragma mark - Canvas view


class PcView : public BView {
public:
	PcView(BRect frame)
		:
		BView(frame, "canvas", B_FOLLOW_ALL_SIDES,
			B_WILL_DRAW | B_FRAME_EVENTS | B_INPUT_METHOD_AWARE),
		fBitmap(NULL),
		fScale(1)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		SetDrawingMode(B_OP_COPY);
	}

	~PcView()
	{
		delete fBitmap;
	}

	virtual void Draw(BRect updateRect)
	{
		// The frame is in device pixels (fScale per logical pixel, on an
		// air/OS desktop drawn at a higher density): app_server scales the
		// logical destination back up, so it lands one to one.
		BRect painted;
		if (fBitmap != NULL) {
			BRect logical = fBitmap->Bounds();
			logical.right = (logical.right + 1) / fScale - 1;
			logical.bottom = (logical.bottom + 1) / fScale - 1;
			painted = logical & updateRect;
			if (painted.IsValid()) {
				BRect source(painted.left * fScale, painted.top * fScale,
					(painted.right + 1) * fScale - 1,
					(painted.bottom + 1) * fScale - 1);
				source = source & fBitmap->Bounds();
				DrawBitmapAsync(fBitmap, source, painted);
			}
		}
		// Uncovered parts (the window grew and the next frame isn't there
		// yet) get the panel colour instead of garbage.
		SetHighColor(ui_color(B_PANEL_BACKGROUND_COLOR));
		BRegion rest(updateRect);
		if (painted.IsValid())
			rest.Exclude(painted);
		FillRegion(&rest);
	}

	virtual void MouseDown(BPoint where)
	{
		MakeFocus(true);
		SetMouseEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);
		PcEvent event = MakeEvent(PC_MOUSE_DOWN);
		event.x = where.x;
		event.y = where.y;
		BMessage* message = Window()->CurrentMessage();
		if (message != NULL) {
			message->FindInt32("buttons", &event.buttons);
			message->FindInt32("clicks", &event.clicks);
		}
		Push(event);
	}

	virtual void MouseUp(BPoint where)
	{
		PcEvent event = MakeEvent(PC_MOUSE_UP);
		event.x = where.x;
		event.y = where.y;
		BMessage* message = Window()->CurrentMessage();
		if (message != NULL)
			message->FindInt32("buttons", &event.buttons);
		Push(event);
	}

	virtual void MouseMoved(BPoint where, uint32 transit,
		const BMessage* dragMessage)
	{
		PcEvent event = MakeEvent(PC_MOUSE_MOVED);
		event.x = where.x;
		event.y = where.y;
		BMessage* message = Window()->CurrentMessage();
		if (message != NULL)
			message->FindInt32("buttons", &event.buttons);
		if (transit == B_EXITED_VIEW && event.buttons == 0)
			event.kind = PC_POINTER_GONE;
		Push(event);
	}

	virtual void KeyDown(const char* bytes, int32 numBytes)
	{
		Key(PC_KEY_DOWN, Window()->CurrentMessage(), bytes, numBytes);
	}

	virtual void KeyUp(const char* bytes, int32 numBytes)
	{
		Key(PC_KEY_UP, Window()->CurrentMessage(), bytes, numBytes);
	}

	void Key(int32 kind, BMessage* message, const char* bytes, int32 numBytes)
	{
		PcEvent event = MakeEvent(kind);
		if (message != NULL) {
			message->FindInt32("key", &event.key);
			message->FindInt32("raw_char", &event.raw_char);
			int32 modifiers;
			if (message->FindInt32("modifiers", &modifiers) == B_OK)
				event.modifiers = modifiers;
			int32 repeat;
			if (message->FindInt32("be:key_repeat", &repeat) == B_OK)
				event.repeat = repeat;
		}
		std::string text(bytes != NULL ? bytes : "",
			bytes != NULL && numBytes > 0 ? numBytes : 0);
		event.text_len = text.size();
		Push(event, text);
	}

	virtual void FrameResized(float width, float height)
	{
		PcEvent event = MakeEvent(PC_RESIZED);
		event.x = width + 1;
		event.y = height + 1;
		Push(event);
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->WasDropped() && message->HasRef("refs")) {
			BPoint where = ConvertFromScreen(message->DropPoint());
			PcEvent event = MakeEvent(PC_DROPPED_FILES);
			event.x = where.x;
			event.y = where.y;
			std::string paths = PathsOf(message, true);
			event.text_len = paths.size();
			Push(event, paths);
			return;
		}
		switch (message->what) {
			case B_MOUSE_WHEEL_CHANGED:
			{
				// A wheel's whole steps, or a touchpad's scroll gesture
				// (air/OS): its unrounded steps, and the gesture's phase in
				// clicks (1 began, 2 changed, 3 ended). id is the device's
				// subtype (2: touchpad), repeat 1 for the precise deltas.
				PcEvent event = MakeEvent(PC_WHEEL);
				message->FindInt32("be:device_subtype", &event.id);
				float dx, dy;
				int32 phase;
				if (message->FindInt32("be:wheel_phase", &phase) == B_OK
					&& message->FindFloat("be:wheel_precise_delta_x", &dx) == B_OK
					&& message->FindFloat("be:wheel_precise_delta_y", &dy) == B_OK) {
					event.dx = dx;
					event.dy = dy;
					event.clicks = phase;
					event.repeat = 1;
				} else {
					message->FindFloat("be:wheel_delta_x", &event.dx);
					message->FindFloat("be:wheel_delta_y", &event.dy);
				}
				Push(event);
				break;
			}
			case B_MODIFIERS_CHANGED:
				Push(MakeEvent(PC_MODIFIERS));
				break;
			case B_INPUT_METHOD_EVENT:
				InputMethodEvent(message);
				break;
			default:
				BView::MessageReceived(message);
		}
	}

	// An input method composes text in the interface's own text fields (the
	// view is B_INPUT_METHOD_AWARE): its text while composing is a pre-edit,
	// its confirmed text is typed, and its candidate window goes by the caret.
	void InputMethodEvent(BMessage* message)
	{
		int32 opcode;
		if (message->FindInt32("be:opcode", &opcode) != B_OK)
			return;
		switch (opcode) {
			case B_INPUT_METHOD_STARTED:
			{
				BMessenger method;
				if (message->FindMessenger("be:reply_to", &method) != B_OK)
					break;
				std::lock_guard<std::mutex> lock(sImeLock);
				sImeMethod = method;
				sImeText.clear();
				break;
			}
			case B_INPUT_METHOD_CHANGED:
			{
				const char* string;
				if (message->FindString("be:string", &string) != B_OK)
					break;
				bool confirmed;
				if (message->FindBool("be:confirmed", &confirmed) != B_OK)
					confirmed = false;
				// The pre-edit's selected part (the clause being converted),
				// in bytes: key to raw_char.
				PcEvent event = MakeEvent(PC_IME);
				event.id = confirmed ? PC_IME_COMMIT : PC_IME_PREEDIT;
				event.key = -1;
				event.raw_char = -1;
				int32 start, end;
				if (!confirmed
					&& message->FindInt32("be:selection", 0, &start) == B_OK
					&& message->FindInt32("be:selection", 1, &end) == B_OK) {
					event.key = start;
					event.raw_char = end;
				}
				std::string text(string);
				{
					// Only in a transaction: not after the interface stopped
					// it (pc_host_ime_stop), as BTextView.
					std::lock_guard<std::mutex> lock(sImeLock);
					if (!sImeMethod.IsValid())
						break;
					sImeText = confirmed ? std::string() : text;
				}
				event.text_len = text.size();
				Push(event, text);
				break;
			}
			case B_INPUT_METHOD_STOPPED:
			{
				{
					std::lock_guard<std::mutex> lock(sImeLock);
					sImeMethod = BMessenger();
					sImeText.clear();
				}
				PcEvent event = MakeEvent(PC_IME);
				event.id = PC_IME_STOPPED;
				Push(event);
				break;
			}
			case B_INPUT_METHOD_LOCATION_REQUEST:
			{
				BMessenger method;
				BRect caret;
				int32 count;
				{
					std::lock_guard<std::mutex> lock(sImeLock);
					method = sImeMethod;
					count = CountUtf8Characters(sImeText);
					caret = sImeCaret;
					if (!sImeCaretKnown) {
						// Before the interface has said: the view's corner.
						font_height height;
						GetFontHeight(&height);
						caret.Set(0, 0, 0, ceilf(height.ascent + height.descent));
					}
				}
				if (!method.IsValid())
					break;
				// A point per character, all the caret's (the interface
				// doesn't say where each one is drawn), on the screen: the
				// canvas view's own conversion counts the menu bar above it.
				BMessage reply(B_INPUT_METHOD_EVENT);
				reply.AddInt32("be:opcode", B_INPUT_METHOD_LOCATION_REQUEST);
				BPoint where = ConvertToScreen(caret.LeftTop());
				for (int32 i = 0; i < std::max(count, (int32)1); i++) {
					reply.AddPoint("be:location_reply", where);
					reply.AddFloat("be:height_reply", caret.Height());
				}
				method.SendMessage(&reply);
				break;
			}
		}
	}

	// Called with the window locked. The pixels are device pixels, scale
	// per logical pixel.
	void Present(const uint32_t* pixels, int32 width, int32 height,
		int32 x0, int32 y0, int32 x1, int32 y1, float scale)
	{
		if (scale != fScale) {
			fScale = scale;
			delete fBitmap;
			fBitmap = NULL;
		}
		if (fBitmap == NULL || fBitmap->Bounds().IntegerWidth() + 1 != width
			|| fBitmap->Bounds().IntegerHeight() + 1 != height) {
			delete fBitmap;
			fBitmap = new BBitmap(BRect(0, 0, width - 1, height - 1), B_RGB32);
			if (fBitmap->InitCheck() != B_OK) {
				delete fBitmap;
				fBitmap = NULL;
				return;
			}
			x0 = 0;
			y0 = 0;
			x1 = width;
			y1 = height;
		}
		if (x0 < 0)
			x0 = 0;
		if (y0 < 0)
			y0 = 0;
		if (x1 > width)
			x1 = width;
		if (y1 > height)
			y1 = height;
		if (x1 <= x0 || y1 <= y0)
			return;
		uint8* bits = (uint8*)fBitmap->Bits();
		int32 bytesPerRow = fBitmap->BytesPerRow();
		for (int32 y = y0; y < y1; y++) {
			memcpy(bits + (size_t)y * bytesPerRow + (size_t)x0 * 4,
				pixels + (size_t)y * width + x0, (size_t)(x1 - x0) * 4);
		}
		Invalidate(BRect(floorf(x0 / fScale), floorf(y0 / fScale),
			ceilf(x1 / fScale) - 1, ceilf(y1 / fScale) - 1));
	}

	// Called with the window locked: the rectangles ([x0, y0, x1, y1) device
	// pixels, four values each) of a frame read back from the GPU. While the
	// window puts frames straight into the screen they are drawn at once and
	// waited for: an update drawn later could put this frame over a newer one
	// the GPU has put there in between.
	void PresentRects(const uint32_t* pixels, int32 width, int32 height,
		const int32_t* rects, int32 count, float scale, bool direct)
	{
		const int32_t all[4] = { 0, 0, width, height };
		if (scale != fScale || fBitmap == NULL
			|| fBitmap->Bounds().IntegerWidth() + 1 != width
			|| fBitmap->Bounds().IntegerHeight() + 1 != height) {
			// A new bitmap: all of it.
			fScale = scale;
			delete fBitmap;
			fBitmap = new BBitmap(BRect(0, 0, width - 1, height - 1), B_RGB32);
			if (fBitmap->InitCheck() != B_OK) {
				delete fBitmap;
				fBitmap = NULL;
				return;
			}
			rects = all;
			count = 1;
		}
		uint8* bits = (uint8*)fBitmap->Bits();
		int32 bytesPerRow = fBitmap->BytesPerRow();
		for (int32 i = 0; i < count; i++) {
			int32 x0 = std::max(rects[i * 4], (int32_t)0);
			int32 y0 = std::max(rects[i * 4 + 1], (int32_t)0);
			int32 x1 = std::min(rects[i * 4 + 2], width);
			int32 y1 = std::min(rects[i * 4 + 3], height);
			if (x1 <= x0 || y1 <= y0)
				continue;
			for (int32 y = y0; y < y1; y++) {
				memcpy(bits + (size_t)y * bytesPerRow + (size_t)x0 * 4,
					pixels + (size_t)y * width + x0, (size_t)(x1 - x0) * 4);
			}
			BRect logical(floorf(x0 / fScale), floorf(y0 / fScale),
				ceilf(x1 / fScale) - 1, ceilf(y1 / fScale) - 1);
			if (direct) {
				BRect source(logical.left * fScale, logical.top * fScale,
					(logical.right + 1) * fScale - 1,
					(logical.bottom + 1) * fScale - 1);
				DrawBitmapAsync(fBitmap, source & fBitmap->Bounds(), logical);
			} else
				Invalidate(logical);
		}
		if (direct)
			Sync();
	}

private:
	BBitmap* fBitmap;
	float fScale;
};


// #pragma mark - Menu bar


// One line of a menu bar description from src/menu.rs: its fields, split at
// the unit separator. "M label" and "S label" start a menu and a submenu, "E"
// ends one, "-" is a separator, and "I command label flags char modifiers" an
// item (flags: 1 enabled, 2 marked; char: the shortcut's character code, 0 for
// none; modifiers: Haiku's modifier bits).
static std::vector<std::vector<std::string> >
ParseMenuSpec(const std::string& spec)
{
	std::vector<std::vector<std::string> > lines;
	size_t start = 0;
	while (start < spec.size()) {
		size_t end = spec.find('\n', start);
		if (end == std::string::npos)
			end = spec.size();
		std::vector<std::string> fields;
		size_t field = start;
		while (true) {
			size_t next = spec.find('\x1f', field);
			if (next == std::string::npos || next > end)
				next = end;
			fields.push_back(spec.substr(field, next - field));
			if (next >= end)
				break;
			field = next + 1;
		}
		if (!fields.empty() && !fields[0].empty())
			lines.push_back(fields);
		start = end + 1;
	}
	return lines;
}


static int32
FieldInt(const std::vector<std::string>& fields, size_t index)
{
	return index < fields.size() ? atoi(fields[index].c_str()) : 0;
}


static const char*
FieldText(const std::vector<std::string>& fields, size_t index)
{
	return index < fields.size() ? fields[index].c_str() : "";
}


// #pragma mark - Window


class PcWindow : public BDirectWindow {
public:
	PcWindow(BRect frame, const char* title, bool menuBar)
		:
		BDirectWindow(frame, title, B_TITLED_WINDOW,
			B_ASYNCHRONOUS_CONTROLS | DirectPresentFlags()),
		fMenuBar(NULL),
		fTracking(false),
		fPendingMenu(false),
		fPendingRebuild(false)
	{
		// The menus come from the interface (pc_host_menu_set); the bar is
		// here from the start, so the window opens at its final size.
		if (menuBar) {
			fMenuBar = new BMenuBar(BRect(0, 0, Bounds().Width(), 0), "menu bar");
			AddChild(fMenuBar);
			// It takes its height from the menu font only when first laid
			// out; the canvas goes under it now.
			float width, height;
			fMenuBar->GetPreferredSize(&width, &height);
			fMenuBar->ResizeTo(Bounds().Width(), height);
		}
		BRect canvas = Bounds();
		canvas.top = CanvasTop();
		fView = new PcView(canvas);
		AddChild(fView);
		fView->MakeFocus(true);
		sCanvasTop = canvas.top;
		// The interface has its own Cut, Copy, Paste, Select All and Close
		// (with a Save Changes prompt), so these keys reach it as keys.
		RemoveShortcut('W', B_COMMAND_KEY);
		RemoveShortcut('X', B_COMMAND_KEY);
		RemoveShortcut('C', B_COMMAND_KEY);
		RemoveShortcut('V', B_COMMAND_KEY);
		RemoveShortcut('A', B_COMMAND_KEY);
	}

	virtual bool QuitRequested()
	{
		if (sAllowQuit)
			return true;
		Push(MakeEvent(PC_CLOSE));
		return false;
	}

	virtual void DispatchMessage(BMessage* message, BHandler* handler)
	{
		// BWindow eats every Command+key it has no shortcut for; the
		// interface's shortcuts (Command+B for bold…) need them. Command+Q
		// still quits the Haiku way.
		if (message->what == B_KEY_DOWN) {
			int32 modifiers = 0;
			int32 rawChar = 0;
			const char* bytes = NULL;
			message->FindInt32("modifiers", &modifiers);
			message->FindInt32("raw_char", &rawChar);
			message->FindString("bytes", &bytes);
			// Command+Escape opens the menu bar the Haiku way.
			bool menuKey = fMenuBar != NULL && rawChar == B_ESCAPE;
			if ((modifiers & B_COMMAND_KEY) != 0 && rawChar != 'q'
				&& rawChar != 'Q' && !menuKey) {
				fView->Key(PC_KEY_DOWN, message, bytes,
					bytes != NULL ? strlen(bytes) : 0);
				return;
			}
		}
		BWindow::DispatchMessage(message, handler);
	}

	virtual void WindowActivated(bool active)
	{
		PcEvent event = MakeEvent(PC_FOCUS);
		event.x = active ? 1 : 0;
		Push(event);
	}

	virtual void FrameMoved(BPoint where)
	{
		PcEvent event = MakeEvent(PC_MOVED);
		event.x = where.x;
		event.y = where.y;
		Push(event);
	}

	virtual void Minimize(bool minimize)
	{
		BWindow::Minimize(minimize);
		PcEvent event = MakeEvent(PC_MINIMIZED);
		event.x = minimize ? 1 : 0;
		Push(event);
	}

	virtual void ScreenChanged(BRect frame, color_space mode)
	{
		Push(MakeEvent(PC_COLORS));
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case B_COLORS_UPDATED:
				Push(MakeEvent(PC_COLORS));
				break;
			case B_INPUT_METHOD_EVENT:
				// Input methods address the focus view; what is sent to the
				// window goes there too.
				fView->InputMethodEvent(message);
				break;
			case B_FONTS_UPDATED:
				// The menu bar's height follows the menu font.
				Relayout();
				BWindow::MessageReceived(message);
				break;
			case kMenuChosen:
			{
				const char* command;
				if (message->FindString(kMenuCommandField, &command) == B_OK) {
					PcEvent event = MakeEvent(PC_MENU);
					std::string text(command);
					event.text_len = text.size();
					Push(event, text);
				}
				break;
			}
			case kMenuApply:
				if (!fTracking && fPendingMenu) {
					fPendingMenu = false;
					ApplyMenu(fPendingSpec, fPendingRebuild);
					fPendingSpec.clear();
					fPendingRebuild = false;
				}
				break;
			default:
				BWindow::MessageReceived(message);
		}
	}

	virtual void MenusBeginning()
	{
		fTracking = true;
		BDirectWindow::MenusBeginning();
	}

	virtual void MenusEnded()
	{
		fTracking = false;
		BDirectWindow::MenusEnded();
		// After the tracking thread is done with the menus.
		if (fPendingMenu)
			PostMessage(kMenuApply);
	}

	// Called with the window locked: brings the menu bar in step with a
	// description (ParseMenuSpec). Rebuilt when its structure changed (or the
	// description doesn't fit the items there), otherwise labels, enabled and
	// marked updated in place. While a menu is open, the newest description
	// waits until it closes.
	void SetMenu(const std::string& spec, bool rebuild)
	{
		if (fMenuBar == NULL)
			return;
		if (fTracking) {
			fPendingSpec = spec;
			fPendingRebuild = fPendingRebuild || rebuild;
			fPendingMenu = true;
			return;
		}
		ApplyMenu(spec, rebuild);
	}

	bool Tracking() const { return fTracking; }

	// The canvas's top: under the menu bar while it shows.
	float CanvasTop() const
	{
		// IsHidden() alone is also true while the window is (before Show()).
		if (fMenuBar == NULL || fMenuBar->IsHidden(fMenuBar))
			return 0;
		return fMenuBar->Frame().bottom + 1;
	}

	// Puts the canvas under the menu bar (or at the top when there is none or
	// it is hidden).
	void Relayout()
	{
		float top = CanvasTop();
		BRect frame = fView->Frame();
		if (frame.top != top) {
			fView->MoveTo(0, top);
			fView->ResizeTo(Bounds().Width(), Bounds().Height() - top);
		}
		sCanvasTop = top;
	}

	void ShowMenuBar(bool show)
	{
		if (fMenuBar == NULL || fMenuBar->IsHidden(fMenuBar) == !show)
			return;
		if (show)
			fMenuBar->Show();
		else
			fMenuBar->Hide();
		Relayout();
	}

	PcView* View() const { return fView; }

	// BDirectWindow's Zoom(BPoint, float, float) hides BWindow's Zoom().
	using BWindow::Zoom;

	// The direct daemon thread, without the window lock: app_server waits
	// here (half a second at most) before it moves, covers or hides the
	// window, and the presenter returns once the GPU has finished drawing
	// where the window was. With B_DIRECT_DEVICE_PIXELS (air/OS) the bounds
	// and clipping are frame buffer pixels.
	virtual void DirectConnected(direct_buffer_info* info)
	{
		std::lock_guard<std::mutex> lock(sDirectLock);
		bool usable = (info->buffer_state & B_DIRECT_MODE_MASK)
				!= B_DIRECT_STOP
			&& info->device_scale >= 100 && info->bits_per_pixel == 32
			&& (info->pixel_format == B_RGB32
				|| info->pixel_format == B_RGBA32)
			&& info->layout == B_BUFFER_NONINTERLEAVED
			&& info->orientation == B_BUFFER_TOP_TO_BOTTOM
			&& info->clip_list_count <= kMaxDirectRects;
		sDirect.connected = usable ? 1 : 0;
		sDirect.rects = sDirectRects;
		sDirect.rectCount = 0;
		if (usable) {
			sDirect.bounds[0] = info->window_bounds.left;
			// The canvas, under the menu bar (device pixels).
			sDirect.bounds[1] = info->window_bounds.top
				+ (int32_t)roundf(sCanvasTop * info->device_scale / 100.0f);
			sDirect.bounds[2] = info->window_bounds.right;
			sDirect.bounds[3] = info->window_bounds.bottom;
			sDirect.bytesPerRow = info->bytes_per_row;
			sDirect.rectCount = info->clip_list_count;
			for (uint32 i = 0; i < info->clip_list_count; i++) {
				sDirectRects[i * 4] = info->clip_list[i].left;
				sDirectRects[i * 4 + 1] = info->clip_list[i].top;
				sDirectRects[i * 4 + 2] = info->clip_list[i].right;
				sDirectRects[i * 4 + 3] = info->clip_list[i].bottom;
			}
		}
		sDirectReported = true;
		if (sDirectSink != NULL)
			sDirectSink(&sDirect);
	}

private:
	void ApplyMenu(const std::string& spec, bool rebuild)
	{
		std::vector<std::vector<std::string> > lines = ParseMenuSpec(spec);
		if (!rebuild && !UpdateMenu(lines))
			rebuild = true;
		if (rebuild)
			BuildMenu(lines);
		Relayout();
	}

	// Labels, enabled and marked in place; false when the items there don't
	// match the description.
	bool UpdateMenu(const std::vector<std::vector<std::string> >& lines)
	{
		size_t next = 0;
		for (size_t i = 0; i < lines.size(); i++) {
			const std::vector<std::string>& line = lines[i];
			char kind = line[0][0];
			if (kind != 'M' && kind != 'S' && kind != 'I')
				continue;
			if (next >= fItems.size())
				return false;
			BMenuItem* item = fItems[next++];
			const char* label = FieldText(line, kind == 'I' ? 2 : 1);
			if (strcmp(item->Label(), label) != 0)
				item->SetLabel(label);
			if (kind == 'I') {
				int32 flags = FieldInt(line, 3);
				if (item->IsEnabled() != ((flags & 1) != 0))
					item->SetEnabled((flags & 1) != 0);
				if (item->IsMarked() != ((flags & 2) != 0))
					item->SetMarked((flags & 2) != 0);
			}
		}
		return next == fItems.size();
	}

	void BuildMenu(const std::vector<std::vector<std::string> >& lines)
	{
		fMenuBar->RemoveItems(0, fMenuBar->CountItems(), true);
		fItems.clear();
		std::vector<BMenu*> open;
		for (size_t i = 0; i < lines.size(); i++) {
			const std::vector<std::string>& line = lines[i];
			BMenu* parent = open.empty() ? (BMenu*)fMenuBar : open.back();
			switch (line[0][0]) {
				case 'M':
				case 'S':
				{
					BMenu* menu = new BMenu(FieldText(line, 1));
					BMenuItem* item = new BMenuItem(menu);
					parent->AddItem(item);
					fItems.push_back(item);
					open.push_back(menu);
					break;
				}
				case 'E':
					if (!open.empty())
						open.pop_back();
					break;
				case '-':
					if (parent != fMenuBar)
						parent->AddSeparatorItem();
					break;
				case 'I':
				{
					BMessage* message = new BMessage(kMenuChosen);
					message->AddString(kMenuCommandField, FieldText(line, 1));
					int32 flags = FieldInt(line, 3);
					BMenuItem* item = new BMenuItem(FieldText(line, 2), message,
						(char)FieldInt(line, 4), (uint32)FieldInt(line, 5));
					item->SetEnabled((flags & 1) != 0);
					item->SetMarked((flags & 2) != 0);
					parent->AddItem(item);
					fItems.push_back(item);
					break;
				}
			}
		}
	}

	PcView* fView;
	BMenuBar* fMenuBar;
	// Every menu, submenu and item, in the description's order.
	std::vector<BMenuItem*> fItems;
	bool fTracking;
	bool fPendingMenu;
	bool fPendingRebuild;
	std::string fPendingSpec;
};


static PcWindow* sWindow = NULL;


// The frame of the monitor most of the window is on. On air/OS one desktop
// spans every monitor (BScreen's frame is all of them); its display layout
// knows each one. Elsewhere, the screen.
static BRect
MonitorFrame(BWindow* window)
{
	typedef status_t (*GetDisplayFrame)(BRect, bool, BRect&);
	static GetDisplayFrame getFrame = (GetDisplayFrame)dlsym(RTLD_DEFAULT,
		"_ZN8BPrivate17get_display_frameE5BRectbRS0_");
	BRect display;
	if (getFrame != NULL && getFrame(window->Frame(), false, display) == B_OK
		&& display.IsValid())
		return display;
	return BScreen(window).Frame();
}


// #pragma mark - File panels


// Shows the files a file panel offers: folders, and files with one of the
// extensions (all files when there are none, or one is "*").
class ExtensionFilter : public BRefFilter {
public:
	ExtensionFilter(const std::vector<std::string>& extensions)
		:
		fAll(false)
	{
		for (size_t i = 0; i < extensions.size(); i++) {
			BString extension(extensions[i].c_str());
			extension.ToLower();
			if (extension == "*") {
				fExtensions.clear();
				fAll = true;
				return;
			}
			if (extension.Length() > 0)
				fExtensions.push_back(extension);
		}
		fAll = fExtensions.empty();
	}

	virtual bool Filter(const entry_ref* ref, BNode* node,
		struct stat_beos* stat, const char* mimeType)
	{
		if (fAll)
			return true;
		BEntry entry(ref, true);
		if (entry.IsDirectory())
			return true;
		BString name(ref->name);
		name.ToLower();
		for (size_t i = 0; i < fExtensions.size(); i++) {
			BString suffix(".");
			suffix << fExtensions[i];
			if (name.EndsWith(suffix))
				return true;
		}
		return false;
	}

private:
	std::vector<BString> fExtensions;
	bool fAll;
};


// A panel's filters, the dialog's in its order: each one's label and the
// files it shows. Panels don't own their ref filters; these do.
struct PanelFilters {
	std::vector<std::string> labels;
	std::vector<ExtensionFilter*> filters;

	void Clear()
	{
		for (size_t i = 0; i < filters.size(); i++)
			delete filters[i];
		labels.clear();
		filters.clear();
	}
};


// The filters of a description from src/host.rs: one per line, its label then
// its extensions, separated by the unit separator. None: one showing every file,
// without a label.
static PanelFilters
MakeFilters(const char* spec)
{
	PanelFilters made;
	std::string text(spec != NULL ? spec : "");
	std::vector<std::vector<std::string> > lines = ParseMenuSpec(text);
	for (size_t i = 0; i < lines.size(); i++) {
		std::vector<std::string> extensions(lines[i].begin() + 1, lines[i].end());
		made.labels.push_back(lines[i][0]);
		made.filters.push_back(new ExtensionFilter(extensions));
	}
	if (made.filters.empty())
		made.filters.push_back(new ExtensionFilter(std::vector<std::string>()));
	return made;
}


static BFilePanel* sOpenPanel = NULL;
static PanelFilters sOpenFilters;
static BFilePanel* sSavePanel = NULL;
static PanelFilters sSaveFilters;
static int32 sDialogId = -1;
static bool sDialogAnswered = true;


static void
AnswerDialog(int32 id, bool picked, const std::string& path)
{
	if (id != sDialogId || sDialogAnswered)
		return;
	sDialogAnswered = true;
	PcEvent event = MakeEvent(PC_DIALOG_ANSWER);
	event.id = id;
	event.x = picked ? 1 : 0;
	event.text_len = path.size();
	Push(event, path);
}


// #pragma mark - Application


static void ChooseFilter(const BMessage* message);


class PcApp : public BApplication {
public:
	PcApp(const char* signature, status_t* error)
		:
		BApplication(signature, error),
		fReady(false)
	{
	}

	virtual void ReadyToRun()
	{
		fReady = true;
	}

	virtual void RefsReceived(BMessage* message)
	{
		int32 dialog;
		if (message->FindInt32(kDialogField, &dialog) == B_OK) {
			std::string path = PathsOf(message, true);
			AnswerDialog(dialog, !path.empty(), path);
			return;
		}
		PcEvent event = MakeEvent(PC_OPEN_FILES);
		std::string paths = PathsOf(message, true);
		event.text_len = paths.size();
		if (!paths.empty())
			Push(event, paths);
	}

	virtual void ArgvReceived(int32 argc, char** argv)
	{
		// The Rust side reads its own command line. Another launch's, handed
		// over as this copy is already running (B_SINGLE_LAUNCH), opens its
		// documents here: the arguments that aren't options (nor an option's
		// value), relative to the folder it was started in.
		// BApplication posts this copy's own arguments to itself before
		// B_READY_TO_RUN, without a reply address, so IsSourceRemote()
		// can't tell them from another launch's.
		BMessage* message = CurrentMessage();
		if (!fReady || message == NULL || !message->IsSourceRemote())
			return;
		const char* cwd = NULL;
		message->FindString("cwd", &cwd);
		std::string paths;
		for (int32 i = 1; i < argc; i++) {
			if (strncmp(argv[i], "--", 2) == 0) {
				// Every option but --version, --help and --new-window takes
				// a value (src/main.rs).
				if (strcmp(argv[i], "--new-window") != 0
					&& strcmp(argv[i], "--version") != 0
					&& strcmp(argv[i], "--help") != 0)
					i++;
				continue;
			}
			if (argv[i][0] == '-' && argv[i][1] != '\0')
				continue;
			BPath path;
			if (argv[i][0] != '/' && cwd != NULL)
				path.SetTo(cwd, argv[i], true);
			else
				path.SetTo(argv[i], NULL, true);
			if (path.InitCheck() != B_OK)
				continue;
			if (!paths.empty())
				paths += '\n';
			paths += path.Path();
		}
		// Even without documents (only options): the running copy's window
		// comes to the front, as for a launch without arguments
		// (B_SILENT_RELAUNCH).
		PcEvent event = MakeEvent(PC_OPEN_FILES);
		event.text_len = paths.size();
		Push(event, paths);
	}

	virtual bool QuitRequested()
	{
		if (sAllowQuit)
			return BApplication::QuitRequested();
		Push(MakeEvent(PC_QUIT_REQUESTED));
		return false;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case B_SAVE_REQUESTED:
			{
				int32 dialog;
				entry_ref directory;
				const char* name;
				if (message->FindInt32(kDialogField, &dialog) != B_OK)
					break;
				if (message->FindRef("directory", &directory) != B_OK
					|| message->FindString("name", &name) != B_OK) {
					AnswerDialog(dialog, false, std::string());
					break;
				}
				BPath path(&directory);
				path.Append(name);
				AnswerDialog(dialog, path.InitCheck() == B_OK,
					path.InitCheck() == B_OK ? path.Path() : "");
				break;
			}
			case B_CANCEL:
				// Sent whenever a panel closes, after its answer if it had
				// one.
				AnswerDialog(sDialogId, false, std::string());
				break;
			case kFilterChosen:
				ChooseFilter(message);
				break;
			case B_SILENT_RELAUNCH:
				// A launch without arguments while this copy runs (the
				// Deskbar, Tracker's icon): BApplication only activates the
				// team, which leaves a minimized window down. The window
				// comes forward as for opened documents.
				Push(MakeEvent(PC_OPEN_FILES));
				BApplication::MessageReceived(message);
				break;
			default:
				BApplication::MessageReceived(message);
		}
	}

private:
	// B_READY_TO_RUN has come: B_ARGV_RECEIVED is another launch's.
	bool fReady;
};


// #pragma mark - Clipboard


static void*
CopyOut(const void* data, size_t length, size_t* _length)
{
	void* copy = malloc(length > 0 ? length : 1);
	if (copy == NULL)
		return NULL;
	memcpy(copy, data, length);
	*_length = length;
	return copy;
}


// #pragma mark - C interface


extern "C" {


// Creates the application: 0, 1 when a copy started from the same file is
// already running (B_SINGLE_LAUNCH) and has been handed this launch's
// arguments, or -1.
int32_t
pc_host_init(const char* signature)
{
	if (be_app != NULL)
		return 0;
	// Not deleted when it failed: the process ends.
	status_t error = B_OK;
	new PcApp(signature, &error);
	if (error == B_ALREADY_RUNNING)
		return 1;
	return error == B_OK && be_app != NULL ? 0 : -1;
}


// Opens the window: its frame's top-left corner at (x, y) (centred on the
// screen when x is negative), content size width × height.
int32_t
pc_host_open_window(const char* title, float x, float y, float width,
	float height, float minWidth, float minHeight, int32_t zoomed,
	int32_t menuBar)
{
	if (sWindow != NULL)
		return 0;
	BRect frame(0, 0, width - 1, height - 1);
	BScreen screen;
	BRect screenFrame = screen.Frame();
	if (x >= 0 && y >= 0)
		frame.OffsetTo(x, y);
	sWindow = new PcWindow(frame, title, menuBar != 0);
	// The sizes are the canvas's: the menu bar comes on top.
	float bar = sWindow->CanvasTop();
	if (bar > 0) {
		sWindow->ResizeBy(0, bar);
		frame.bottom += bar;
		minHeight += bar;
	}
	sWindow->SetSizeLimits(minWidth - 1, 32768, minHeight - 1, 32768);
	if (x < 0 || y < 0 || !screenFrame.Intersects(frame)) {
		// A default size larger than the monitor (a small or dense display)
		// shrinks to fit it, with room for the tab, borders and Deskbar.
		BRect monitor = MonitorFrame(sWindow);
		float maxWidth = monitor.Width() * 0.92f;
		float maxHeight = monitor.Height() * 0.88f;
		if (frame.Width() > maxWidth || frame.Height() > maxHeight) {
			sWindow->ResizeTo(std::max(std::min(frame.Width(), maxWidth),
					minWidth - 1),
				std::max(std::min(frame.Height(), maxHeight), minHeight - 1));
		}
		sWindow->CenterOnScreen();
	}
	else {
		// (x, y) is where the frame's corner goes, tab and border included.
		BRect outer = sWindow->DecoratorFrame();
		sWindow->MoveBy(frame.left - outer.left, frame.top - outer.top);
	}
	if (zoomed)
		sWindow->Zoom();
	sWindow->Show();
	return 0;
}


void
pc_host_run()
{
	be_app->Run();
	delete be_app;
}


void
pc_host_quit()
{
	sAllowQuit = true;
	be_app->PostMessage(B_QUIT_REQUESTED);
}


// Waits until an event is queued, pc_host_wake is called or the timeout
// (microseconds; negative: none) passes. Returns how many events are queued.
int32_t
pc_host_wait(int64_t timeout)
{
	std::unique_lock<std::mutex> lock(sQueueLock);
	auto ready = [] { return !sQueue.empty() || sWoken; };
	if (timeout < 0)
		sQueueCondition.wait(lock, ready);
	else if (timeout > 0)
		sQueueCondition.wait_for(lock, std::chrono::microseconds(timeout), ready);
	sWoken = false;
	return (int32_t)sQueue.size();
}


void
pc_host_wake()
{
	{
		std::lock_guard<std::mutex> lock(sQueueLock);
		sWoken = true;
	}
	sQueueCondition.notify_all();
}


// Takes the next event, if any (1) or returns 0. Its text stays readable
// through pc_host_event_text until the next call.
int32_t
pc_host_next_event(PcEvent* event)
{
	std::lock_guard<std::mutex> lock(sQueueLock);
	if (sQueue.empty())
		return 0;
	*event = sQueue.front().event;
	sCurrentText = sQueue.front().text;
	event->text_len = sCurrentText.size();
	sQueue.pop_front();
	return 1;
}


const char*
pc_host_event_text()
{
	return sCurrentText.c_str();
}


void
pc_host_present(const uint32_t* pixels, int32_t width, int32_t height,
	int32_t x0, int32_t y0, int32_t x1, int32_t y1, float scale)
{
	if (sWindow == NULL || width <= 0 || height <= 0 || scale <= 0
		|| !sWindow->Lock())
		return;
	sWindow->View()->Present(pixels, width, height, x0, y0, x1, y1, scale);
	sWindow->Unlock();
}


// The rectangles ([x0, y0, x1, y1) device pixels, four values each) of a
// frame read back from the GPU.
void
pc_host_present_rects(const uint32_t* pixels, int32_t width, int32_t height,
	const int32_t* rects, int32_t count, float scale)
{
	if (sWindow == NULL || width <= 0 || height <= 0 || scale <= 0
		|| count <= 0 || !sWindow->Lock())
		return;
	bool direct;
	{
		std::lock_guard<std::mutex> lock(sDirectLock);
		direct = sDirect.connected != 0;
	}
	sWindow->View()->PresentRects(pixels, width, height, rects, count, scale,
		direct);
	sWindow->Unlock();
}


// Where the window's direct connection is reported (NULL: nowhere). The last
// one is reported at once; unsetting waits for a report in progress.
void
pc_host_direct_sink(PcDirectSink sink)
{
	std::lock_guard<std::mutex> lock(sDirectLock);
	sDirectSink = sink;
	if (sink != NULL && sDirectReported)
		sink(&sDirect);
}


void
pc_host_view_size(float* width, float* height)
{
	*width = 0;
	*height = 0;
	if (sWindow == NULL || !sWindow->Lock())
		return;
	BRect bounds = sWindow->View()->Bounds();
	*width = bounds.IntegerWidth() + 1;
	*height = bounds.IntegerHeight() + 1;
	sWindow->Unlock();
}


void
pc_host_window_info(PcWindowInfo* info)
{
	memset(info, 0, sizeof(*info));
	if (sWindow == NULL || !sWindow->Lock())
		return;
	BRect inner = sWindow->Frame();
	// The interface's content is the canvas, under the menu bar.
	inner.top += sWindow->CanvasTop();
	BRect outer = sWindow->DecoratorFrame();
	BRect screen = MonitorFrame(sWindow);
	info->inner_left = inner.left;
	info->inner_top = inner.top;
	info->inner_right = inner.right + 1;
	info->inner_bottom = inner.bottom + 1;
	info->outer_left = outer.left;
	info->outer_top = outer.top;
	info->outer_right = outer.right + 1;
	info->outer_bottom = outer.bottom + 1;
	info->screen_width = screen.Width() + 1;
	info->screen_height = screen.Height() + 1;
	info->minimized = sWindow->IsMinimized();
	info->active = sWindow->IsActive();
	// Haiku doesn't say whether a window is zoomed.
	info->zoomed = 0;
	info->fullscreen = sFullscreen;
	sWindow->Unlock();
}


void
pc_host_set_title(const char* title)
{
	if (sWindow == NULL || !sWindow->Lock())
		return;
	if (strcmp(sWindow->Title(), title) != 0)
		sWindow->SetTitle(title);
	sWindow->Unlock();
}


// The window commands of src/host.rs's `WindowCommand`.
void
pc_host_window_command(int32_t command, float a, float b)
{
	if (sWindow == NULL || !sWindow->Lock())
		return;
	switch (command) {
		case 1:
		{
			// Move the frame's top-left corner (decorator included) to (a, b).
			BRect inner = sWindow->Frame();
			BRect outer = sWindow->DecoratorFrame();
			sWindow->MoveTo(a + inner.left - outer.left, b + inner.top - outer.top);
			break;
		}
		case 2:
			// The canvas's size: the menu bar comes on top.
			sWindow->ResizeTo(a - 1, b - 1 + sWindow->CanvasTop());
			break;
		case 3:
			sWindow->Minimize(a != 0);
			break;
		case 4:
			// Zoom toggles: only zoom, never un-zoom on request.
			if (a != 0)
				sWindow->Zoom();
			break;
		case 5:
			sWindow->Activate(true);
			break;
		case 6:
		{
			float minWidth, maxWidth, minHeight, maxHeight;
			sWindow->GetSizeLimits(&minWidth, &maxWidth, &minHeight, &maxHeight);
			sWindow->SetSizeLimits(a - 1, maxWidth, b - 1 + sWindow->CanvasTop(),
				maxHeight);
			break;
		}
        case 7:
        {
            bool on = a != 0;
            if (on == sFullscreen) break;
            if (on) {
                sWindowedFrame = sWindow->Frame();
                BRect screen = MonitorFrame(sWindow);
                sWindow->SetLook(B_NO_BORDER_WINDOW_LOOK);
                sWindow->SetFeel(B_FLOATING_ALL_WINDOW_FEEL);
                // Full screen is the document alone, as on the Mac.
                sWindow->ShowMenuBar(false);
                sWindow->MoveTo(screen.LeftTop());
                sWindow->ResizeTo(screen.Width(), screen.Height());
            } else {
                sWindow->SetFeel(B_NORMAL_WINDOW_FEEL);
                sWindow->SetLook(B_TITLED_WINDOW_LOOK);
                sWindow->ShowMenuBar(true);
                sWindow->MoveTo(sWindowedFrame.LeftTop());
                sWindow->ResizeTo(sWindowedFrame.Width(), sWindowedFrame.Height());
            }
            sFullscreen = on;
            break;
        }

	}
	sWindow->Unlock();
}


// Brings the menu bar in step with a description (PcWindow::SetMenu):
// rebuilt when `rebuild` says its structure changed.
void
pc_host_menu_set(const char* spec, size_t length, int32_t rebuild)
{
	if (sWindow == NULL || !sWindow->Lock())
		return;
	sWindow->SetMenu(std::string(spec, length), rebuild != 0);
	sWindow->Unlock();
}


// Whether one of the menu bar's menus is open.
int32_t
pc_host_menu_tracking()
{
	if (sWindow == NULL || !sWindow->Lock())
		return 0;
	bool tracking = sWindow->Tracking();
	sWindow->Unlock();
	return tracking ? 1 : 0;
}


void
pc_host_set_cursor(int32_t id)
{
	static int32_t sCurrent = -1;
	static BCursor* sCursors[32] = {};
	if (id < 1 || id > 31 || sWindow == NULL)
		id = B_CURSOR_ID_SYSTEM_DEFAULT;
	if (id == sCurrent)
		return;
	if (sCursors[id] == NULL)
		sCursors[id] = new BCursor((BCursorID)id);
	if (!sWindow->Lock())
		return;
	sWindow->View()->SetViewCursor(sCursors[id]);
	sWindow->Unlock();
	sCurrent = id;
}


uint32_t
pc_host_modifiers()
{
	return modifiers();
}


// The interface scale: the system font size against Haiku's default 12 pt.
float
pc_host_ui_scale()
{
	float scale = be_plain_font->Size() / 12.0f;
	if (scale < 1.0f)
		scale = 1.0f;
	if (scale > 4.0f)
		scale = 4.0f;
	return scale;
}


// How many frame buffer pixels a logical pixel is: air/OS draws the desktop
// at its monitors' density (the "render scale" of its display layout, in
// percent). 1 on a Haiku without the call.
float
pc_host_device_scale()
{
	typedef status_t (*GetLayout)(BMessage&);
	static GetLayout getLayout = (GetLayout)dlsym(RTLD_DEFAULT,
		"_ZN8BPrivate18get_display_layoutER8BMessage");
	if (getLayout == NULL)
		return 1;
	BMessage layout;
	if (getLayout(layout) != B_OK)
		return 1;
	int32 best = 0;
	BMessage display;
	for (int32 i = 0; layout.FindMessage("display", i, &display) == B_OK; i++) {
		int32 scale;
		if (display.GetBool("enabled", true)
			&& display.FindInt32("render scale", &scale) == B_OK && scale > best)
			best = scale;
	}
	if (best < 100 || best > 400)
		return 1;
	return best / 100.0f;
}


// Whether the panel colour (Appearance preferences) is dark.
int32_t
pc_host_dark()
{
	rgb_color color = ui_color(B_PANEL_BACKGROUND_COLOR);
	return color.Brightness() < 128 ? 1 : 0;
}


// The system's formatting conventions (en_US) and the preferred languages
// (en, de…, one per line), for the locale variables the interface reads.
char*
pc_host_locale(char** languages)
{
	BFormattingConventions conventions;
	BString id;
	BLocaleRoster* roster = BLocaleRoster::Default();
	if (roster != NULL) {
		const BLocale* locale = roster->GetDefaultLocale();
		if (locale != NULL && locale->GetFormattingConventions(&conventions) == B_OK)
			id = conventions.ID();
	}
	BString list;
	BMessage preferred;
	if (roster != NULL && roster->GetPreferredLanguages(&preferred) == B_OK) {
		const char* language;
		for (int32 i = 0; preferred.FindString("language", i, &language) == B_OK; i++) {
			if (list.Length() > 0)
				list << "\n";
			list << language;
		}
	}
	*languages = strdup(list.String());
	return strdup(id.String());
}


void
pc_free(void* data)
{
	free(data);
}


// The clipboard's data of one MIME type (malloc'd; NULL when absent).
void*
pc_clipboard_get(const char* type, size_t* length)
{
	*length = 0;
	void* copy = NULL;
	if (!be_clipboard->Lock())
		return NULL;
	BMessage* clip = be_clipboard->Data();
	const void* data;
	ssize_t size;
	if (clip != NULL && clip->FindData(type, B_MIME_TYPE, &data, &size) == B_OK
		&& size >= 0)
		copy = CopyOut(data, size, length);
	be_clipboard->Unlock();
	return copy;
}




// Replaces the clipboard with text and, when html isn't NULL, its HTML.
int32_t
pc_clipboard_set(const char* text, size_t textLength, const char* html,
	size_t htmlLength)
{
	if (!be_clipboard->Lock())
		return -1;
	be_clipboard->Clear();
	BMessage* clip = be_clipboard->Data();
	status_t status = B_ERROR;
	if (clip != NULL) {
		status = clip->AddData("text/plain", B_MIME_TYPE, text, textLength);
		if (status == B_OK && html != NULL)
			status = clip->AddData("text/html", B_MIME_TYPE, html, htmlLength);
		if (status == B_OK)
			status = be_clipboard->Commit();
	}
	be_clipboard->Unlock();
	return status == B_OK ? 0 : -1;
}


// Puts an encoded picture (PNG) on the clipboard.
int32_t
pc_clipboard_set_picture(const char* type, const void* data, size_t length)
{
	if (!be_clipboard->Lock())
		return -1;
	be_clipboard->Clear();
	BMessage* clip = be_clipboard->Data();
	status_t status = B_ERROR;
	if (clip != NULL) {
		status = clip->AddData(type, B_MIME_TYPE, data, length);
		if (status == B_OK)
			status = be_clipboard->Commit();
	}
	be_clipboard->Unlock();
	return status == B_OK ? 0 : -1;
}




// Keep file panels on the same monitor as the document window.
static void
PositionPanel(BFilePanel* panel)
{
    if (sWindow == NULL || !sWindow->Lock()) return;
    BRect parent = sWindow->Frame();
    BRect screen = MonitorFrame(sWindow);
    sWindow->Unlock();
    BWindow* window = panel->Window();
    if (!window->Lock()) return;
    BRect frame = window->Frame();
    float x = parent.left + (parent.Width() - frame.Width()) / 2;
    float y = parent.top + (parent.Height() - frame.Height()) / 2;
    window->MoveTo(std::max(screen.left, std::min(x, screen.right - frame.Width())),
        std::max(screen.top + 25, std::min(y, screen.bottom - frame.Height())));
    window->Unlock();
}

}


// Gives a panel its filters (MakeFilters), the first one showing, with a menu
// to switch between them beside its folder menu when they have labels: Haiku's
// file panel has none of its own. `owned` keeps them; the ones it had go once
// the panel no longer uses them.
static void
SetPanelFilters(BFilePanel* panel, PanelFilters& owned, PanelFilters filters,
	bool save)
{
	panel->SetRefFilter(filters.filters[0]);
	owned.Clear();
	owned = filters;
	BWindow* window = panel->Window();
	if (!window->Lock())
		return;
	float spacing = be_control_look->ComposeSpacing(B_USE_SMALL_SPACING);
	BView* background = window->ChildAt(0);
	BView* folders = window->FindView("DirMenuField");
	BMenuField* folderField = dynamic_cast<BMenuField*>(folders);
	BView* old = window->FindView(kFilterMenuName);
	if (old != NULL) {
		old->RemoveSelf();
		delete old;
		// The folder menu's own width again (TFilePanel's).
		if (folderField != NULL) {
			folderField->ResizeTo(spacing * 50, folderField->Bounds().Height());
			folderField->MenuBar()->SetMaxContentWidth(spacing * 50 - 26);
		}
	}
	if (background != NULL && folders != NULL && !owned.labels.empty()) {
		// At the right of the folder menu's row: as wide as the longest
		// label and the menu's arrow, up to half the row; the folder menu
		// gives up the room.
		float widest = 0;
		for (size_t i = 0; i < owned.labels.size(); i++) {
			widest = std::max(widest,
				be_plain_font->StringWidth(owned.labels[i].c_str()));
		}
		BRect frame = folders->Frame();
		float rowLeft = frame.left;
		frame.right = background->Bounds().right - spacing;
		float width = std::min(widest + 3 * frame.Height(),
			(frame.right - rowLeft - spacing) / 2);
		frame.left = frame.right - width;
		float foldersWidth = frame.left - spacing - rowLeft;
		folders->ResizeTo(foldersWidth, folders->Bounds().Height());
		if (folderField != NULL)
			folderField->MenuBar()->SetMaxContentWidth(foldersWidth - 26);
		BPopUpMenu* menu = new BPopUpMenu("filters");
		for (size_t i = 0; i < owned.labels.size(); i++) {
			BMessage* message = new BMessage(kFilterChosen);
			message->AddInt32(kFilterField, (int32)i);
			message->AddBool("pc:save", save);
			BMenuItem* item = new BMenuItem(owned.labels[i].c_str(), message);
			item->SetTarget(be_app);
			item->SetMarked(i == 0);
			menu->AddItem(item);
		}
		BMenuField* field = new BMenuField(frame, kFilterMenuName, NULL, menu,
			B_FOLLOW_RIGHT | B_FOLLOW_TOP);
		field->SetDivider(0);
		field->MenuBar()->SetMaxContentWidth(width - 26);
		background->AddChild(field);
	}
	window->Unlock();
}


// A filter chosen in a panel's file type menu.
static void
ChooseFilter(const BMessage* message)
{
	int32 index;
	if (message->FindInt32(kFilterField, &index) != B_OK || index < 0)
		return;
	bool save = message->GetBool("pc:save", false);
	BFilePanel* panel = save ? sSavePanel : sOpenPanel;
	PanelFilters& filters = save ? sSaveFilters : sOpenFilters;
	if (panel != NULL && (size_t)index < filters.filters.size())
		panel->SetRefFilter(filters.filters[index]);
}


extern "C" {


// Shows a file panel: Open (save == 0, 2 for a folder, 3 for several files) or
// Save suggesting the name, with the filters (MakeFilters) to choose from, the
// first one showing. Its answer arrives as a PC_DIALOG_ANSWER event with the id.
int32_t
pc_host_file_panel(int32_t id, int32_t save, const char* name,
	const char* directory, const char* filters, const char* title)
{
	bool inDirectory = directory != NULL && directory[0] != '\0';
	BMessenger target(be_app);
	if (save == 1) {
		if (sSavePanel == NULL) {
			sSavePanel = new BFilePanel(B_SAVE_PANEL, &target, NULL, 0, false,
				NULL, NULL, false, true);
		}
		BMessage message(B_SAVE_REQUESTED);
		message.AddInt32(kDialogField, id);
		sSavePanel->SetMessage(&message);
		sSavePanel->SetSaveText(name != NULL ? name : "");
		SetPanelFilters(sSavePanel, sSaveFilters, MakeFilters(filters), true);
		if (inDirectory)
			sSavePanel->SetPanelDirectory(directory);
		if (sSavePanel->Window()->Lock()) {
			sSavePanel->Window()->SetTitle(title);
			sSavePanel->Window()->Unlock();
		}
		sDialogId = id;
		sDialogAnswered = false;
		PositionPanel(sSavePanel);
		sSavePanel->Show();
		return 0;
	}
	delete sOpenPanel;
	sOpenPanel = new BFilePanel(B_OPEN_PANEL, &target, NULL,
        save == 2 ? B_DIRECTORY_NODE : B_FILE_NODE, save == 3, NULL, NULL, false, true);
	SetPanelFilters(sOpenPanel, sOpenFilters, MakeFilters(filters), false);
	BMessage message(B_REFS_RECEIVED);
	message.AddInt32(kDialogField, id);
	sOpenPanel->SetMessage(&message);
	if (inDirectory)
		sOpenPanel->SetPanelDirectory(directory);
	if (sOpenPanel->Window()->Lock()) {
		sOpenPanel->Window()->SetTitle(title);
		sOpenPanel->Window()->Unlock();
	}
	sOpenPanel->Refresh();
	sDialogId = id;
	sDialogAnswered = false;
	PositionPanel(sOpenPanel);
	sOpenPanel->Show();
	return 0;
}


// Where the interface's caret is, for an input method's candidate window
// (canvas view coordinates, logical pixels), or that there's no text to type
// into (known 0).
void
pc_host_ime_caret(int32_t known, float left, float top, float right,
	float bottom)
{
	std::lock_guard<std::mutex> lock(sImeLock);
	sImeCaretKnown = known != 0;
	sImeCaret.Set(left, top, right, bottom);
}


// Ends the input method's transaction, if one is in progress: the interface
// interrupted the composition (the caret moved away, the field lost the
// keyboard).
void
pc_host_ime_stop()
{
	BMessenger method;
	{
		std::lock_guard<std::mutex> lock(sImeLock);
		method = sImeMethod;
		sImeMethod = BMessenger();
		sImeText.clear();
	}
	if (!method.IsValid())
		return;
	BMessage message(B_INPUT_METHOD_EVENT);
	message.AddInt32("be:opcode", B_INPUT_METHOD_STOPPED);
	method.SendMessage(&message);
}


int32_t
pc_host_open_url(const char* url)
{
	BUrl target(url, true);
	if (!target.IsValid())
		return -1;
	return target.OpenWithPreferredApplication(false) == B_OK ? 0 : -1;
}




// Makes the application the preferred one for the types (one per line) that
// have none yet, so Tracker opens PDFs with it out of the box; a
// preferred application the user chose stays.
int32_t
pc_host_claim_types(const char* signature, const char* types)
{
	BString list(types);
	int32 claimed = 0;
	int32 start = 0;
	while (start < list.Length()) {
		int32 end = list.FindFirst('\n', start);
		if (end < 0)
			end = list.Length();
		BString type;
		list.CopyInto(type, start, end - start);
		start = end + 1;
		BMimeType mime(type.String());
		if (mime.InitCheck() != B_OK)
			continue;
		char preferred[B_MIME_TYPE_LENGTH];
		if (mime.IsInstalled() && mime.GetPreferredApp(preferred) == B_OK
			&& preferred[0] != '\0')
			continue;
		if (!mime.IsInstalled())
			mime.Install();
		if (mime.SetPreferredApp(signature) == B_OK)
			claimed++;
	}
	return claimed;
}


// Gives a file the system saved for us its MIME type (by extension and
// content), as files saved by Haiku programs have, so Tracker opens it with
// the right application. Leaves a type already set alone.
int32_t
pc_host_type_file(const char* path)
{
	return update_mime_info(path, false, true, B_UPDATE_MIME_INFO_NO_FORCE)
		>= B_OK ? 0 : -1;
}


void
pc_host_alert(const char* title, const char* text)
{
	BAlert* alert = new BAlert(title, text, "OK", NULL, NULL,
		B_WIDTH_AS_USUAL, B_STOP_ALERT);
	alert->Go();
}


}


// #pragma mark - Key Store


extern "C" {


// The Key Store keyring the application's secrets go in.
static const char* kKeyring = "Digital IDs";


static void
AppendField(std::string& out, const void* data, size_t length)
{
	uint32_t size = (uint32_t)length;
	out.append((const char*)&size, sizeof(size));
	out.append((const char*)data, length);
}


// The keyring's generic keys (malloc'd, *length bytes): identifier, secondary
// identifier and data of each, every field a native-endian 32-bit length and its
// bytes. NULL when the keyring can't be read (*status says why; a keyring that
// doesn't exist yet is empty).
void*
pc_keystore_list(size_t* length, int32_t* status)
{
	*length = 0;
	BKeyStore store;
	std::string out;
	uint32 cookie = 0;
	BKey key;
	status_t result;
	while ((result = store.GetNextKey(kKeyring, B_KEY_TYPE_GENERIC,
			B_KEY_PURPOSE_ANY, cookie, key)) == B_OK) {
		AppendField(out, key.Identifier(), strlen(key.Identifier()));
		AppendField(out, key.SecondaryIdentifier(),
			strlen(key.SecondaryIdentifier()));
		AppendField(out, key.Data(), key.DataLength());
	}
	// B_ENTRY_NOT_FOUND: past the last key; B_BAD_VALUE at once: no such
	// keyring yet.
	bool none = result == B_BAD_VALUE && cookie == 0;
	*status = result == B_ENTRY_NOT_FOUND || none ? B_OK : result;
	if (*status != B_OK)
		return NULL;
	return CopyOut(out.data(), out.size(), length);
}


// Keeps a secret in the keyring (made if it isn't there), replacing one with
// the same identifier. The Key Store asks the user once for each kind of
// access, so the one there is only looked up when adding finds it.
int32_t
pc_keystore_add(const char* identifier, const char* name, const void* data,
	size_t length)
{
	BKeyStore store;
	status_t result = store.AddKeyring(kKeyring);
	if (result != B_OK && result != B_NAME_IN_USE)
		return result;
	BKey key(B_KEY_PURPOSE_GENERIC, identifier, name, (const uint8*)data,
		length);
	result = store.AddKey(kKeyring, key);
	if (result != B_NAME_IN_USE)
		return result;
	BKey old;
	result = store.GetKey(kKeyring, B_KEY_TYPE_GENERIC, identifier, old);
	if (result == B_OK)
		result = store.RemoveKey(kKeyring, old);
	return result == B_OK ? store.AddKey(kKeyring, key) : result;
}


int32_t
pc_keystore_remove(const char* identifier)
{
	BKeyStore store;
	BKey key;
	status_t result = store.GetKey(kKeyring, B_KEY_TYPE_GENERIC, identifier,
		key);
	if (result != B_OK)
		return result;
	return store.RemoveKey(kKeyring, key);
}


const char*
pc_error_string(int32_t status)
{
	return strerror(status);
}


}


// Tracker's copy mode for a file on the clipboard (kCopySelectionTo in its
// Commands.h): a paste there copies it.
static const int32 kTrackerCopy = 'Tcsl';


// Puts files on the clipboard the ways air/OS programs paste them: as Tracker
// copies them (a ref named "r<device>_<node>" with its mode "m<device>_<node>"),
// as "refs", as text/uri-list (one file: URI per line, `uris`) and as their
// paths in text/plain.
extern "C" int32_t
pc_clipboard_files(const char* paths, const char* uris)
{
	if (!be_clipboard->Lock())
		return -1;
	be_clipboard->Clear();
	BMessage* clip = be_clipboard->Data();
	status_t status = B_ERROR;
	if (clip != NULL) {
		status = B_OK;
		BString list(paths);
		int32 start = 0;
		while (status == B_OK && start < list.Length()) {
			int32 end = list.FindFirst('\n', start);
			if (end < 0)
				end = list.Length();
			BString path;
			list.CopyInto(path, start, end - start);
			start = end + 1;
			entry_ref ref;
			node_ref node;
			BEntry entry(path.String());
			if (entry.GetRef(&ref) != B_OK || entry.GetNodeRef(&node) != B_OK)
				continue;
			BString name;
			name.SetToFormat("r%" B_PRIdDEV "_%" B_PRIdINO, node.device,
				node.node);
			BString mode(name);
			mode.SetByteAt(0, 'm');
			status = clip->AddRef(name.String(), &ref);
			if (status == B_OK)
				status = clip->AddInt32(mode.String(), kTrackerCopy);
			if (status == B_OK)
				status = clip->AddRef("refs", &ref);
		}
		if (status == B_OK) {
			status = clip->AddData("text/uri-list", B_MIME_TYPE, uris,
				strlen(uris));
		}
		if (status == B_OK) {
			status = clip->AddData("text/plain", B_MIME_TYPE, paths,
				strlen(paths));
		}
		if (status == B_OK)
			status = be_clipboard->Commit();
	}
	be_clipboard->Unlock();
	return status == B_OK ? 0 : -1;
}


// The paths of the files on the clipboard, one per line (malloc'd; NULL when
// there are none): every entry_ref there, as Tracker copies files ("r…"
// fields) or as "refs", each file once.
extern "C" char*
pc_clipboard_paths()
{
	if (!be_clipboard->Lock())
		return NULL;
	std::vector<std::string> paths;
	BMessage* clip = be_clipboard->Data();
	char* name;
	type_code type;
	int32 count;
	for (int32 i = 0; clip != NULL
			&& clip->GetInfo(B_REF_TYPE, i, &name, &type, &count) == B_OK; i++) {
		entry_ref ref;
		for (int32 j = 0; j < count && clip->FindRef(name, j, &ref) == B_OK;
				j++) {
			BPath path(&ref);
			if (path.InitCheck() == B_OK
				&& std::find(paths.begin(), paths.end(), path.Path())
					== paths.end())
				paths.push_back(path.Path());
		}
	}
	be_clipboard->Unlock();
	if (paths.empty())
		return NULL;
	std::string text;
	for (size_t i = 0; i < paths.size(); i++) {
		if (i > 0)
			text += '\n';
		text += paths[i];
	}
	return strdup(text.c_str());
}
