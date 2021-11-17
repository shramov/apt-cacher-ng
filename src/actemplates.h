#ifndef ACTEMPLATES_H
#define ACTEMPLATES_H

#include <functional>

namespace acng {

using tAction = std::function<void()>;
using tCancelableAction = std::function<void(bool)>;

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
	tAction m_p = tAction();
	TFinalAction() =default;
	explicit TFinalAction(tAction xp)
		: m_p(xp)
	{

	}
	~TFinalAction() { if (m_p) m_p(); m_p = tAction(); }
	TFinalAction(const TFinalAction&) = delete;
	TFinalAction(TFinalAction&& other)
	{
		if (& m_p == & other.m_p)
			return;
		reset();
		m_p = other.m_p;
		other.m_p = tAction();
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

template<typename T>
struct RangeLoopAdapter
{
	RangeLoopAdapter(unsigned count, T * elements[])
		: m_start(elements), m_count(count)
	{
	}
	char **begin() const { return m_start; }
	char **end() const { return m_start + m_count; }
private:
	T **m_start;
	unsigned m_count;
};

}


#endif // ACTEMPLATES_H
