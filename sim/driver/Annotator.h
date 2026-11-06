// Simulator-only design notes: right-click the panel, type what it should do, and the
// note lands in annotations/ANNOTATIONS.md with a marked screenshot and the UI context.
// Lives in sim/ so it can never reach a device build.
#pragma once

#include <SDL.h>
#include <cstdint>
#include <vector>

namespace Annotator {

bool isOpen();
// frameRGB24 is the current frame (width * height * 3), captured before the dialog is drawn.
void open(int x, int y, std::vector<uint8_t> frameRGB24, int width, int height);
// Returns true when the dialog consumed the event (typing must not reach the brew/steam keys).
bool handleEvent(const SDL_Event &e);

} // namespace Annotator
