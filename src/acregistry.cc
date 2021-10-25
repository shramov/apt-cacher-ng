#include "acregistry.h"
#include "debug.h"
#include "meta.h"
#include "acfg.h"
#include "evabase.h"

#include <list>

#define IN_ABOUT_ONE_DAY 100000

using namespace std;

namespace acng
{

class TFileItemRegistry : public IFileItemRegistry
{
public:
	tFiGlobMap mapItems;

	struct TExpiredEntry
	{
		TFileItemHolder hodler;
		time_t timeExpired;
	};

	std::list<TExpiredEntry> prolongedLifetimeQ;
	TFileItemHolder Create(cmstring &sPathUnescaped, ESharingHow how, const fileitem::tSpecialPurposeAttr &spattr) override;
	TFileItemHolder Register(tFileItemPtr spCustomFileItem) override;
	time_t BackgroundCleanup() override;
	void dump_status() override;
	void AddToProlongedQueue(TFileItemHolder &&, time_t expTime) override;

	// IFileItemRegistry interface
public:
	void Unreg(fileitem& item) override
	{
		auto pin(as_lptr(&item));
		mapItems.erase(item.m_globRef);
		item.m_globRef = mapItems.end();
		item.m_owner = nullptr;
	}
};

lint_ptr<IFileItemRegistry> SetupServerItemRegistry()
{
	if (!g_registry)
		g_registry = as_lptr((IFileItemRegistry*) new TFileItemRegistry);
	return g_registry;
}

void TeardownServerItemRegistry()
{
	g_registry.reset();
}

void fileitem::Abandon()
{
	LOGSTARTFUNC;
	auto manger = as_lptr(m_owner);
	auto local_ptr = as_lptr(this); // might disappear, clean on stack unwinding

#ifdef DEBUG
	if (m_status > fileitem::FIST_INITED &&	m_status < fileitem::FIST_COMPLETE)
	{
		LOG(mstring("users gone while downloading?: ") + ltos(m_status));
	}
#endif

	// some file items will be held ready for some time
	if (manger
			&& !evabase::in_shutdown
			&& acng::cfg::maxtempdelay
			&& IsVolatile()
			&& m_status == fileitem::FIST_COMPLETE)
	{
		auto now = GetTime();
		auto expDate = time_t(acng::cfg::maxtempdelay)
				+ m_nTimeDlStarted ? m_nTimeDlStarted : now;

		if (expDate > now)
		{
			if (manger)
			{
				// increase the usecount again and keep it for another life
				manger->AddToProlongedQueue(TFileItemHolder(local_ptr), expDate);
			}
			return;
		}
	}

	// nothing, let's put the item into shutdown state
	if (m_status < fileitem::FIST_COMPLETE)
		m_status = fileitem::FIST_DLSTOP;
	m_responseStatus.msg = "Cache file item expired";
	m_responseStatus.code = 500;
	NotifyObservers();

	if (manger)
	{
		LOG("*this is last entry, deleting dl/fi mapping");
		manger->Unreg(*local_ptr);
	}
}

event* regCleanEvent = nullptr;
void cbRunRegClean(evutil_socket_t, short, void *p)
{
	auto pRe = (TFileItemRegistry*) p;
	auto tNext = pRe->BackgroundCleanup();
	if (tNext < END_OF_TIME)
	{
		timeval span { tNext - GetTime(), 1 };
		event_add(regCleanEvent, &span);
	}
}

void TFileItemRegistry::AddToProlongedQueue(TFileItemHolder&& p, time_t expTime)
{
	if (!regCleanEvent)
		regCleanEvent = evtimer_new(evabase::base, cbRunRegClean, this);
	// act like the item is still in use
	prolongedLifetimeQ.emplace_back(TExpiredEntry {move(p), expTime});
	if (prolongedLifetimeQ.size() == 1)
	{
		timeval span { expTime - GetTime(), 1 };
		event_add(regCleanEvent, &span);
	}
}

TFileItemHolder TFileItemRegistry::Create(cmstring &sPathUnescaped, ESharingHow how, const fileitem::tSpecialPurposeAttr& spattr)
{
	LOGSTARTFUNCxs(sPathUnescaped, int(how));

	try
	{
		mstring sPathRel(TFileitemWithStorage::NormalizePath(sPathUnescaped));
		LOG("Normalized: " << sPathRel );
		auto regnew = [&]()
		{
			LOG("Registering as NEW file item...");
			auto sp = as_lptr((fileitem*) new TFileitemWithStorage(sPathRel));
			sp->m_spattr = spattr;
			sp->m_owner = this;
			auto res = mapItems.emplace(sPathRel, sp);
			ASSERT(res.second);
			sp->m_globRef = res.first;
			return TFileItemHolder(sp);
		};

		auto it = mapItems.find(sPathRel);
		if (it == mapItems.end())
			return regnew();

		auto &fi = it->second;

		auto share = [&]()
		{
			LOG("Sharing existing file item");
			return TFileItemHolder(it->second);
		};

		if (how == ESharingHow::ALWAYS_TRY_SHARING || fi->m_bCreateItemMustDisplace)
			return share();

		// detect items that got stuck somehow and move it out of the way
		auto now(GetTime());
		auto makeWay = false;
		if (how == ESharingHow::FORCE_MOVE_OUT_OF_THE_WAY)
			makeWay = true;
		else
		{
			dbgline;
			makeWay = now > (fi->m_nTimeDlStarted + cfg::stucksecs);
			// check the additional conditions for being perfectly identical
			if (!makeWay && fi->IsVolatile() != spattr.bVolatile)
			{
				// replace if previous was working in solid mode because it does less checks
				makeWay = ! fi->IsVolatile();
			}
			if (!makeWay && spattr.bHeadOnly != fi->IsHeadOnly())
			{
				// one is HEAD-only request, the other not, keep the variant in the index which gets more data, the other becomes disposable
				dbgline;
				if (fi->IsHeadOnly())
					makeWay = true;
				else
				{
					// new item is head-only but this is almost worthless, so the existing one wins and new one goes to the side track
					auto sp = make_lptr<TFileitemWithStorage>(sPathRel);
					sp->m_spattr = spattr;
					sp->m_spattr.bNoStore = true;
					return TFileItemHolder(static_lptr_cast<fileitem>(sp));
				}
			}
			if (!makeWay && spattr.nRangeLimit != fi->GetRangeLimit())
			{
				dbgline;
				makeWay = true;
			}
			// XXX: TODO - add validation when remote credentials are supported
		}
		if (!makeWay)
			return share();

		// okay, have to move a probably existing cache file out of the way,
		// therefore needing this evasive maneuver
		auto replPathRel = fi->m_sPathRel + "." + ltos(now);
		auto replPathAbs = SABSPATH(replPathRel);
		auto pathAbs = SABSPATH(fi->m_sPathRel);

		// XXX: this check is crap and cannot happen but better double-check!
		if (AC_UNLIKELY(fi->m_sPathRel.empty()))
			return TFileItemHolder();

		auto abandon_replace = [&]() {
			fi->m_sPathRel = replPathAbs;
			fi->m_eDestroy = fileitem::EDestroyMode::ABANDONED;
			fi->m_globRef = mapItems.end();
			fi->m_owner = nullptr;
			mapItems.erase(it);
			return regnew();
		};

		if (0 == link(pathAbs.c_str(), replPathAbs.c_str()))
		{
			// only if it was actually there!
			if (0 == unlink(pathAbs.c_str()) || errno != ENOENT)
				return abandon_replace();
			else // unlink failed but file was there
				USRERR("Failure to erase stale file item for "sv << pathAbs << " - errno: "sv << tErrnoFmter());
		}
		else
		{
			if (ENOENT == errno) // XXX: replPathAbs doesn't exist but ignore for now
				return abandon_replace();

			USRERR("Failure to move file "sv << pathAbs << " out of the way or cannot create "sv  << replPathAbs << " - errno: "sv << tErrnoFmter());
		}
	}
	catch (std::bad_alloc&)
	{
	}
	return TFileItemHolder();
}

// make the fileitem globally accessible
TFileItemHolder TFileItemRegistry::Register(tFileItemPtr spCustomFileItem)
{
	LOGSTARTFUNCxs(spCustomFileItem->m_sPathRel);

	if (!spCustomFileItem || spCustomFileItem->m_sPathRel.empty())
		return TFileItemHolder();
	// cook a new entry than
	auto installed = mapItems.emplace(spCustomFileItem->m_sPathRel,
									  spCustomFileItem);
	dbgline;
	if(!installed.second)
		return TFileItemHolder(); // conflict, another agent is already active
	dbgline;
	spCustomFileItem->m_globRef = installed.first;
	spCustomFileItem->m_owner = this;
	return TFileItemHolder(spCustomFileItem.get());
}

// this method is supposed to be awaken periodically and detects items with ref count manipulated by
// the request storm prevention mechanism. Items shall be be dropped after some time if no other
// thread but us is using them.
time_t TFileItemRegistry::BackgroundCleanup()
{
	auto now = GetTime();
	LOGSTARTFUNCsx(now);
	// where the destructors eventually do their job on stack unrolling
	decltype(prolongedLifetimeQ) releasedQ;
	if (prolongedLifetimeQ.empty())
		return END_OF_TIME;
	auto notExpired = std::find_if(prolongedLifetimeQ.begin(), prolongedLifetimeQ.end(),
								   [now](const TExpiredEntry &el) {	return el.timeExpired > now;});
	// grab all before expired element, or even all
	releasedQ.splice(releasedQ.begin(), prolongedLifetimeQ, prolongedLifetimeQ.begin(), notExpired);
	return prolongedLifetimeQ.empty() ? END_OF_TIME : prolongedLifetimeQ.front().timeExpired;
}

void TFileItemRegistry::dump_status()
{
	tSS fmt;
	log::err("File descriptor table:\n");
	for (const auto& item : mapItems)
	{
		fmt.clear();
		fmt << "FREF: " << item.first << " [" << item.second->__user_ref_cnt() << "]:\n";
		if (! item.second)
		{
			fmt << "\tBAD REF!\n";
			continue;
		}

		fmt << "\t" << item.second->m_sPathRel
			<< "\n\tDlRefCount: " << item.second->m_nDlRefsCount
			<< "\n\tState: " << (int)  item.second->m_status
			<< "\n\tFilePos: " << item.second->m_nIncommingCount << " , "
					//<< item.second->m_nRangeLimit << " , "
			<< item.second->m_nSizeChecked << " , "
					<< item.second->m_nSizeCachedInitial
					<< "\n\tGotAt: " << item.second->m_nTimeDlStarted << "\n\n";

		log::err(fmt.view());
	}
	log::flush();
}

}
