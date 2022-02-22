#include "acpatcher.h"

#include "meta.h"
#include "acbuf.h"
#include "filereader.h"

#include <fstream>

using namespace std;

namespace acng
{

struct tCompStringView
{
	uint32_t offs = 0, size = 0;
};

struct acpatcher::Impl
{

#if SIZEOF_PTR > 4
	typedef deque<tCompStringView> tLineMap;
#define COMP_SVIEW
#else
	typedef deque<string_view> tLineMap;
#endif

	// sticky, needed in to remember last position in subsequent operations
	unsigned long rangeLast = 0, rangeStart = 0;
	tSS errorBuffer;

	inline void patchChunk(tLineMap& data, string_view cmd, tLineMap patch)
	{

#define EFMT (errorBuffer.clean() << "ERROR: " )
#define ETHROW(tss) throw std::runtime_error((tss).c_str())

		if (AC_UNLIKELY(cmd.empty() || data.empty()))
			ETHROW(EFMT << "Bad patch command");

		const char& code = cmd.back();
		bool append = false; // special, paste contents AFTER the line

		switch (code)
		{
		case 'a':
			append = true;
			__just_fall_through;
		case 'd':
		case 'c':
		{
			char *pEnd = nullptr;
			auto n = strtoul(cmd.data(), &pEnd, 10);
			if (!pEnd || cmd.data() == pEnd)
				ETHROW(EFMT << "bad patch range");

			rangeLast = rangeStart = n;

			if (rangeStart > data.size())
				ETHROW(EFMT << "bad range, start: " << rangeStart);

			if (*pEnd == ',')
			{
				n = strtoul(pEnd + 1, &pEnd, 10);
				// command code should follow after!
				if (!pEnd || & code != pEnd)
					ETHROW(EFMT << "bad patch range");
				rangeLast = n;
				if (rangeLast > data.size() || rangeLast < rangeStart)
					ETHROW(EFMT << "bad range, end: " << rangeLast);
			}
			break;
		}
		case '/':
			if (cmd != "s/.//"sv)
				ETHROW(EFMT << "unsupported command: " << cmd);
#ifdef COMP_SVIEW
			data[rangeStart] = { 0, (1u << 31) + 2 };
#else
			data[rangeStart] = ".\n"sv;
#endif
			return;
		default:
			ETHROW(EFMT << "unsupported command: " << cmd);
			break;
		}

#define DIT(offs) (data.begin() + offs)
		if (append)
			data.insert(DIT(rangeStart + 1), patch.begin(), patch.end());
		else
		{
			// non-moving pasting first
			size_t offset(0), pcount(patch.size());
			while(offset < pcount && rangeStart <= rangeLast)
				*(DIT(rangeStart++)) = patch[offset++];
			if (offset >= pcount && rangeStart > rangeLast)
				return;
			// otherwise extra stuff in new or old range
			if (offset < pcount)
				data.insert(DIT(rangeStart), patch.begin() + offset, patch.end());
			else
				data.erase(DIT(rangeStart), DIT(rangeLast + 1));
		}
	}

	/**
 * @brief patch_file
 * @param args Source path (can be compressed??), patch file (must be not uncompressed)
 * @return
 */
	void patch_file(cmstring& sOrig, cmstring& sPatch, cmstring& sResult)
	{
		filereader frBase, frPatch;
		if (!frBase.OpenFile(sOrig))
			ETHROW(EFMT << "Cannot open original file" << sOrig);
		if (!frPatch.OpenFile(sPatch, true))
			ETHROW(EFMT << "Cannot open patch file" << sPatch);

		tLineMap lines, curHunkLines;
#ifdef COMP_SVIEW
		lines.emplace_back(tCompStringView {0, 0}); // dummy entry to avoid -1 calculations because of ed numbering style
#else
		lines.emplace_back(se); // dummy entry to avoid -1 calculations because of ed numbering style
#endif
		auto baseView = frBase.getView();
		auto patchView = frPatch.getView();
#ifdef COMP_SVIEW
		string_view stuff = ".\n"sv;
		LPCSTR poolBaseAddr[4] = { baseView.data(), patchView.data(), stuff.data(), nullptr };
		constexpr uint32_t allF = 0xffffffffu;
		constexpr uint32_t countMask(allF >> 2), maskPatch(1 << 30);
#endif

		tSplitWalkStrict origSplit(baseView, "\n");

		for(auto view : origSplit) // collect, including newline!
		{
#ifdef COMP_SVIEW
			lines.emplace_back(tCompStringView {uint32_t(view.data() - poolBaseAddr[0]),
												((uint32_t) view.size() + 1) & countMask});
#else
			// not exactly the view, include newline!
			lines.emplace_back(view.data(), view.size() + 1);
#endif
		}
		// if file was ended properly, drop the empty extra line, it contains an inaccessible range anyway
		if (frBase.getView().ends_with('\n'))
			lines.pop_back();

		string_view curPatchCmd;
		tSplitWalkStrict patchSplit(patchView, "\n");
		auto execute = [&]()
		{
			patchChunk(lines, curPatchCmd, curHunkLines);
			curHunkLines.clear();
			curPatchCmd = se;
		};
		for (auto line : patchSplit)
		{
			if (!curPatchCmd.empty()) // collecting mode
			{
				if (line == "."sv)
					execute();
				else
				{
#ifdef COMP_SVIEW
					curHunkLines.emplace_back(tCompStringView
											  {
												  uint32_t(line.data() - poolBaseAddr[1]),
												  (((uint32_t) line.size() + 1) & countMask) | maskPatch
											  }); // with \n
#else
					curHunkLines.emplace_back(line.data(), line.length() + 1); // with \n
#endif
				}
				continue;
			}
			// okay, command, which kind?
			curPatchCmd = line;

			if (line.starts_with("w"sv)) // don't care about the name, though
				break;

			// single-line commands?
			if (line.ends_with('d') || line.starts_with('s'))
				execute();
		}

		ofstream res(sResult);
		if (!res.is_open())
			ETHROW(EFMT << "Cannot write patch result to " << sOrig);
		lines.pop_front(); // dummy offset line
		for (const auto& el : lines)
		{
#ifdef COMP_SVIEW
			auto baseAddr = poolBaseAddr[ el.size >> 30 ];
			const auto p = baseAddr + el.offs;
			const auto len = el.size & countMask;
			res.write(p, len);
#else
			res.write(el.data(), el.size());
#endif
		}
		res.flush();
		if (!res.good())
			ETHROW(EFMT << "Error storin patch result in " << sOrig);
	}
};

acpatcher::acpatcher()
{
	m_pImpl = new Impl;
}

acpatcher::~acpatcher()
{
	delete m_pImpl;
}

void acpatcher::Apply(cmstring &sOrig, cmstring &sPatch, cmstring &sResult) throw()
{
	m_pImpl->patch_file(sOrig, sPatch, sResult);
}

}
