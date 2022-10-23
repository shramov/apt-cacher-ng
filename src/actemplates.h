#ifndef ACTEMPLATES_H
#define ACTEMPLATES_H

#include "sut.h"

#include <functional>
#include <utility>

namespace acng
{

using tAction = std::function<void()>;
using tCancelableAction = std::function<void(bool)>;

template<typename T, typename V>
bool InRange(const T&a, const V&val, const T&b)
{
	return val >=a && val <= b;
}

// unique_ptr semantics (almost) on a non-pointer type
template<typename T, void TFreeFunc(T), T inval_default>
struct auto_raii
{
	T m_p;
	auto_raii() : m_p(inval_default) {}
	explicit auto_raii(T xp) : m_p(xp) {}
	~auto_raii() {
		if (m_p != inval_default)
			TFreeFunc(m_p);
	}
	T release() {
		auto ret = m_p;
		m_p = inval_default;
		return ret;
	}
	T get() const { return m_p; }
	T& operator*() { return m_p; }
	auto_raii(const auto_raii&) = delete;
	auto_raii(auto_raii && other)
	{
		if (& m_p == & other.m_p)
			return;
		m_p = other.m_p;
		other.m_p = inval_default;
	}
	auto_raii& reset(auto_raii &&other)
	{
		if (&other == this)
			return *this;
		if (m_p != other.m_p)
		{
			if(valid())
				TFreeFunc(m_p);
			m_p = other.m_p;
		}
		other.m_p = inval_default;
		return *this;
	}
	auto_raii& reset(T rawNew)
	{
		if (m_p == rawNew) // heh?
			return *this;
		if(valid())
			TFreeFunc(m_p);
		m_p = rawNew;
		return *this;
	}
	void swap(auto_raii &other)
	{
		auto p = m_p;
		m_p = other.m_p;
		other.m_p = p;
	}
	void reset()
	{
		if (valid())
			TFreeFunc(m_p);
		m_p = inval_default;
	}
	bool valid() const { return inval_default != m_p;}
};

/**
 * @brief Single-owner function carrier.
 *
 * Runs the action ONCE when the last carrier is eventually destroyed.
 */
struct TFinalAction
{
	SUTPRIVATE:
	tAction m_p;
public:
	TFinalAction() =default;
	explicit TFinalAction(tAction&& xp)
		: m_p(move(xp))
	{
	}
	~TFinalAction()
	{
		if (m_p)
			m_p();
	}
	TFinalAction(const TFinalAction&) = delete;
	TFinalAction(TFinalAction&& other)
	{
		if (this == &other)
			return;
//		if (& m_p == & other.m_p)
//			return;
		reset();
		m_p.swap(other.m_p);
	}
	TFinalAction& reset(TFinalAction &&other)
	{
		if (&other == this)
			return *this;
		reset();
		m_p.swap(other.m_p);
		return *this;
	}
	TFinalAction& operator=(TFinalAction &&other) { return reset(std::move(other)); }
	TFinalAction& reset(tAction rawNew)
	{
		if(m_p)
			m_p();
		m_p = rawNew;
		return *this;
	}
	void swap(TFinalAction &other)
	{
		auto p = m_p;
		m_p = other.m_p;
		other.m_p = p;
	}
	void reset()
	{
		if (m_p)
			m_p();
		m_p = tAction();
	}
	void release()
	{
		m_p = tAction();
	}
	operator bool() const { return m_p.operator bool(); }
};

#if 0
template<typename T>
struct slist
{
	struct spair { T el; spair* next; };
	spair* m_first = nullptr, m_last = nullptr;
	spair* append(T&& el)
	{
			if (m_last)
				return (m_last->next = new spair {move(el), nullptr});
			return (m_first = m_last = new spair {move(el), nullptr});
	}
	spair& front() { return *m_first; }
	spair& back() { return *m_last; }
	bool empty() { return !m_first; }
	void pop_front() { if (!m_first) return; auto xf = m_first->next; delete m_first; m_first = xf; if (!xf) m_last = nullptr; }
};
#endif

}


#endif // ACTEMPLATES_H
