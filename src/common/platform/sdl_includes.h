#ifndef UAM_COMMON_PLATFORM_SDL_INCLUDES_H
#define UAM_COMMON_PLATFORM_SDL_INCLUDES_H

#if __has_include(<SDL.h>)
#include <SDL.h>
#include <SDL_video.h>
#elif __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#include <SDL2/SDL_video.h>
#else
#error "SDL headers not found. Configure CMake so SDL include directories are available."
#endif

#endif // UAM_COMMON_PLATFORM_SDL_INCLUDES_H
