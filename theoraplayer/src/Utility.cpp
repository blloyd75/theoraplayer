/// @file
/// @version 2.0
/// 
/// @section LICENSE
/// 
/// This program is free software; you can redistribute it and/or modify it under
/// the terms of the BSD license: http://opensource.org/licenses/BSD-3-Clause

#include <string>

#include "Exception.h"
#include "theoraplayer.h"
#include "Utility.h"

namespace theoraplayer
{
	std::string str(int i)
	{
		char s[64] = { 0 };
		sprintf(s, "%d", i);
		return std::string(s);
	}

	std::string strf(float i)
	{
		char s[64] = { 0 };
		sprintf(s, "%.3f", i);
		return std::string(s);
	}

	bool stringEndsWith(const std::string& string, const std::string& suffix)
	{
		int thisLength = (int)string.size();
		int stringLength = (int)suffix.size();
		if (stringLength > thisLength)
		{
			return false;
		}
		return (strcmp(string.c_str() + thisLength - stringLength, suffix.c_str()) == 0);
	}

	int potCeil(int value)
	{
		--value;
		value |= value >> 1;
		value |= value >> 2;
		value |= value >> 4;
		value |= value >> 8;
		value |= value >> 16;
		++value;
		return value;
	}

	FILE* openSupportedFormatFile(const std::string& filename, VideoClip::Format& outFormat, std::string& outFullFilename)
	{
		FILE* file = NULL;
		foreach (VideoClip::Format, it, videoClipFormats)
		{
			outFullFilename = filename;
			if (!stringEndsWith(filename, (*it).extension))
			{
				outFullFilename = filename + (*it).extension;
			}
			file = fopen(outFullFilename.c_str(), "rb");
			if (file != NULL)
			{
				outFormat = (*it);
				break;
			}
		}
		return file;
	}

	AudioLanguageMatch checklanguage(const std::string& desired, const std::string& check)
	{
		// This is a simple function to check language match.
		// It won't crash from bad inputs, but may return incorrect matches for malformed inputs
		std::string::size_type p1l = desired.length();
		std::string::size_type p2l = check.length();
		bool stringsmatchlength = (p1l == p2l);
		if (p2l < p1l) p1l = p2l;
		p2l = 0;

		// 2 Chars for language
		if (p2l >= p1l) 
			return LANGUAGE_MATCH_NONE;
		if (desired[p2l] != check[p2l])
			return LANGUAGE_MATCH_NONE;

		++p2l;
		if (p2l >= p1l) 
			return LANGUAGE_MATCH_NONE;
		if (desired[p2l] != check[p2l])
			return LANGUAGE_MATCH_NONE;

		++p2l;
		// We have a matching language code here
		if (p2l >= p1l)
			return stringsmatchlength ? LANGUAGE_MATCH_LANG_CODE_EXACT:LANGUAGE_MATCH_LANG_CODE;
		if (desired[p2l] != check[p2l] || desired[p2l]!='-')
			return LANGUAGE_MATCH_LANG_CODE;

		++p2l;
		if (p2l >= p1l)
			return LANGUAGE_MATCH_LANG_CODE;
		if (desired[p2l] != check[p2l])
			return LANGUAGE_MATCH_LANG_CODE;
		++p2l;
		if (p2l >= p1l)
			return LANGUAGE_MATCH_LANG_CODE;
		if (desired[p2l] != check[p2l])
			return LANGUAGE_MATCH_LANG_CODE;
		return LANGUAGE_MATCH_LANG_AND_COUNTRY;
	}
}
