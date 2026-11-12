// SMF_MagicBullet - standalone SKSE plugin (CommonLibSSE-NG)
// Casting the "Magic Bullet (Power)" lesser power or the "Magic Bullet" hand spell looses ONE homing bullet that
// weaves between the enemies in front of you and strikes them over and over. No bow, no weapon needed.
// Independent of SMF_RadiantBow.dll (shares only the .nif art: rb_bullet_head.nif / rb_bullet_hit.nif).
//
// Not compiled or run by the author of this file - see README.md "first things to check".

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace logger = SKSE::log;
using namespace std::literals;

namespace MB
{
	// ------------------------------------------------------------------ config
	struct Config
	{
		int   hits = 7;               // strikes with one target (further targets add HitsPerTarget each)
		int   hitsPerTarget = 2;
		int   maxHits = 40;
		float damage = 40.0f;         // per strike, before skill scaling
		float skillScale = 1.0f;      // damage x (1 + skillScale * Destruction / 100)
		float stagger = 0.6f;         // 0 = none
		float graze = 1.0f;           // enemies passed through on the way take this x a strike (0 = off)
		float grazeGap = 0.5f;        // s between grazes of the same enemy
		float speed = 3000.0f;        // u/s
		float turnLow = 360.0f;       // deg/s right after a strike (wide arcs)
		float turnHigh = 900.0f;      // deg/s at full tightening
		float turnRamp = 0.35f;       // s from low to high
		float kick = 38.0f;           // deg random sideways kick after each strike
		float hitRadius = 70.0f;      // u
		float range = 3000.0f;        // enemies further than this are ignored
		float coneHalf = 75.0f;       // deg around the view direction
		int   maxTargets = 5;
		float life = 15.0f;           // s
		float hitScale = 1.0f;
		float lastScale = 1.6f;
		float noTargetDist = 1500.0f; // flies this far straight when nothing is in front (0 = does nothing)
		int   grantMode = 1;          // 0 = nothing, 1 = learn both spells at once, 2 = put the two tomes in the inventory once
		bool  debug = false;
	};
	inline Config cfg;

	inline void LoadConfig()
	{
		std::ifstream f("Data/SKSE/Plugins/SMF_MagicBullet.ini");
		std::string   line;
		while (std::getline(f, line)) {
			if (auto c = line.find(';'); c != std::string::npos) line.erase(c);
			auto eq = line.find('=');
			if (eq == std::string::npos) continue;
			auto trim = [](std::string s) {
				s.erase(0, s.find_first_not_of(" \t\r\n"));
				s.erase(s.find_last_not_of(" \t\r\n") + 1);
				return s;
			};
			auto k = trim(line.substr(0, eq));
			auto v = trim(line.substr(eq + 1));
			if (k.empty() || v.empty()) continue;
			try {
				if (k == "Hits") cfg.hits = std::stoi(v);
				else if (k == "HitsPerTarget") cfg.hitsPerTarget = std::stoi(v);
				else if (k == "MaxHits") cfg.maxHits = std::stoi(v);
				else if (k == "Damage") cfg.damage = std::stof(v);
				else if (k == "SkillScale") cfg.skillScale = std::stof(v);
				else if (k == "Stagger") cfg.stagger = std::stof(v);
				else if (k == "Graze") cfg.graze = std::stof(v);
				else if (k == "GrazeGap") cfg.grazeGap = std::stof(v);
				else if (k == "Speed") cfg.speed = std::stof(v);
				else if (k == "TurnLow") cfg.turnLow = std::stof(v);
				else if (k == "TurnHigh") cfg.turnHigh = std::stof(v);
				else if (k == "TurnRamp") cfg.turnRamp = std::stof(v);
				else if (k == "Kick") cfg.kick = std::stof(v);
				else if (k == "HitRadius") cfg.hitRadius = std::stof(v);
				else if (k == "Range") cfg.range = std::stof(v);
				else if (k == "ConeHalf") cfg.coneHalf = std::stof(v);
				else if (k == "MaxTargets") cfg.maxTargets = std::stoi(v);
				else if (k == "Life") cfg.life = std::stof(v);
				else if (k == "HitScale") cfg.hitScale = std::stof(v);
				else if (k == "LastScale") cfg.lastScale = std::stof(v);
				else if (k == "NoTargetDist") cfg.noTargetDist = std::stof(v);
				else if (k == "GrantMode") cfg.grantMode = std::stoi(v);
				else if (k == "Debug") cfg.debug = std::stoi(v) != 0;
			} catch (...) {
				logger::warn("ini: bad value for {}", k);
			}
		}
	}

	// ------------------------------------------------------------------ forms
	constexpr auto ESP = "SMF_MagicBullet.esp"sv;
	constexpr RE::FormID ID_DMGFX = 0x801, ID_DMGSPL = 0x803, ID_POWER = 0x804, ID_HAND = 0x805, ID_TOMEP = 0x806,
						 ID_TOMEH = 0x807, ID_STAT = 0x808, ID_TRIG = 0x800;

	inline RE::SpellItem*      gDamage{ nullptr };
	inline RE::SpellItem*      gPower{ nullptr };
	inline RE::SpellItem*      gHand{ nullptr };
	inline RE::TESBoundObject* gStat{ nullptr };
	inline RE::EffectSetting*  gTrigger{ nullptr };

	inline bool LoadForms()
	{
		auto dh = RE::TESDataHandler::GetSingleton();
		if (!dh) return false;
		gDamage = dh->LookupForm<RE::SpellItem>(ID_DMGSPL, ESP);
		gPower = dh->LookupForm<RE::SpellItem>(ID_POWER, ESP);
		gHand = dh->LookupForm<RE::SpellItem>(ID_HAND, ESP);
		gStat = dh->LookupForm<RE::TESBoundObject>(ID_STAT, ESP);
		gTrigger = dh->LookupForm<RE::EffectSetting>(ID_TRIG, ESP);
		logger::info("forms: damage={} power={} hand={} stat={} trigger={}", (void*)gDamage, (void*)gPower, (void*)gHand, (void*)gStat, (void*)gTrigger);
		if (!gDamage || !gPower || !gHand || !gStat) {
			logger::error("forms missing - is {} enabled?", ESP);
			return false;
		}
		return true;
	}

	// ------------------------------------------------------------------ math helpers
	inline std::mt19937& Rng()
	{
		static std::mt19937 r{ std::random_device{}() };
		return r;
	}
	inline float Rand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(Rng()); }
	constexpr float DEG = 0.017453292f;

	inline RE::NiPoint3 Norm(RE::NiPoint3 v)
	{
		float l = v.Length();
		return l > 1e-5f ? v / l : RE::NiPoint3{ 0, 1, 0 };
	}
	inline RE::NiPoint3 AnyPerp(const RE::NiPoint3& d)
	{
		RE::NiPoint3 a = std::fabs(d.z) < 0.9f ? RE::NiPoint3{ 0, 0, 1 } : RE::NiPoint3{ 1, 0, 0 };
		return Norm(d.Cross(a));
	}
	inline RE::NiPoint3 RotateAbout(const RE::NiPoint3& v, const RE::NiPoint3& axis, float ang)
	{
		float c = std::cos(ang), s = std::sin(ang);
		return v * c + axis.Cross(v) * s + axis * (axis.Dot(v) * (1.0f - c));
	}
	inline RE::NiPoint3 RotateToward(const RE::NiPoint3& dir, const RE::NiPoint3& want, float maxAng)
	{
		float dot = std::clamp(dir.Dot(want), -1.0f, 1.0f);
		float ang = std::acos(dot);
		if (ang <= maxAng) return want;
		RE::NiPoint3 axis = dir.Cross(want);
		float        len = axis.Length();
		axis = len < 1e-4f ? AnyPerp(dir) : axis / len;
		return Norm(RotateAbout(dir, axis, maxAng));
	}
	inline float SegDist(const RE::NiPoint3& a, const RE::NiPoint3& b, const RE::NiPoint3& p)
	{
		RE::NiPoint3 ab = b - a;
		float        l2 = ab.Dot(ab);
		float        t = l2 > 1e-6f ? std::clamp((p - a).Dot(ab) / l2, 0.0f, 1.0f) : 0.0f;
		return (a + ab * t - p).Length();
	}

	// ------------------------------------------------------------------ actors
	inline RE::NiPoint3 Center(RE::Actor* a) { return a->GetPosition() + RE::NiPoint3{ 0, 0, a->GetHeight() * 0.62f }; }

	inline RE::NiPoint3 Forward(RE::PlayerCharacter* p)
	{
		auto  ang = p->GetAngle();  // x = pitch (+ = looking down), z = heading
		float cp = std::cos(ang.x), sp = std::sin(ang.x);
		return { std::sin(ang.z) * cp, std::cos(ang.z) * cp, -sp };
	}

	// ------------------------------------------------------------------ bullet
	struct Bullet
	{
		RE::ObjectRefHandle  ref;
		RE::NiPoint3         pos, dir;
		std::vector<RE::ActorHandle> targets;
		std::map<RE::FormID, float>  grazeCd;
		size_t               idx{ 0 };
		int                  strikesLeft{ 0 };
		float                age{ 0 }, sinceStrike{ 0 }, noTargetLeft{ 0 };
		bool                 done{ false };
	};
	inline std::vector<Bullet> gBullets;
	inline float               gLastCast = -10.0f;
	inline float               gClock = 0.0f;

	inline void Place(RE::TESObjectREFR* r, const RE::NiPoint3& pos)
	{
		r->SetPosition(pos);
		if (auto n = r->Get3D()) {
			n->local.translate = pos;
			RE::NiUpdateData ud;
			n->Update(ud);
		}
	}

	inline void Burst(const RE::NiPoint3& at, float scale)
	{
		auto p = RE::PlayerCharacter::GetSingleton();
		if (!p || !p->GetParentCell() || scale <= 0.0f) return;
		RE::NiMatrix3 m;
		RE::BSTempEffectParticle::Spawn(p->GetParentCell(), 1.5f, "SMF\\RadiantBow\\rb_bullet_hit.nif", m, at, scale, 7, nullptr);
	}

	inline void Hurt(RE::Actor* a, float mult)
	{
		auto p = RE::PlayerCharacter::GetSingleton();
		if (!a || a->IsDead() || !gDamage) return;
		float skill = p->AsActorValueOwner()->GetActorValue(RE::ActorValue::kDestruction);
		float mag = cfg.damage * mult * (1.0f + cfg.skillScale * skill / 100.0f);
		if (auto caster = p->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
			caster->CastSpellImmediate(gDamage, false, a, 1.0f, false, mag, p);
		}
		if (cfg.stagger > 0.0f && !a->IsDead()) {
			a->SetGraphVariableFloat("staggerMagnitude", cfg.stagger);
			a->NotifyAnimationGraph("staggerStart");
		}
	}

	inline void Finish(Bullet& b)
	{
		if (auto r = b.ref.get()) {
			r->Disable();
			r->SetDelete(true);
		}
		b.done = true;
	}

	inline RE::Actor* NextTarget(Bullet& b)
	{
		for (size_t n = 0; n < b.targets.size(); ++n) {
			auto a = b.targets[b.idx % b.targets.size()].get();
			if (a && !a->IsDead() && !a->IsDisabled()) return a.get();
			b.idx++;
		}
		return nullptr;
	}

	inline void Launch(RE::PlayerCharacter* p, bool fromHand)
	{
		logger::info("launch ({})", fromHand ? "hand spell" : "power");
		if (!gStat || !p->GetParentCell()) { logger::error("launch aborted: stat={} cell={}", (void*)gStat, (void*)p->GetParentCell()); return; }
		RE::NiPoint3 fwd = Forward(p);
		RE::NiPoint3 start = p->GetPosition() + RE::NiPoint3{ 0, 0, p->GetHeight() * 0.88f } + fwd * 40.0f;

		if (fromHand) {
			bool left = false;
			if (auto lc = p->GetMagicCaster(RE::MagicSystem::CastingSource::kLeftHand); lc && lc->currentSpell == gHand) left = true;
			bool first = false;
			if (auto cam = RE::PlayerCamera::GetSingleton()) first = cam->IsInFirstPerson();
			if (auto root = p->Get3D(first)) {
				if (auto n = root->GetObjectByName(left ? "NPC L Hand [LHnd]"sv : "NPC R Hand [RHnd]"sv)) {
					start = n->world.translate + fwd * 20.0f;
				}
			}
		}

		// targets: hostile, alive, in range, in front, visible; closest to the aim first
		struct C { RE::ActorHandle h; float ang; };
		std::vector<C> cand;
		if (auto pl = RE::ProcessLists::GetSingleton()) {
			for (auto& h : pl->highActorHandles) {
				auto a = h.get();
				if (!a || a.get() == p || a->IsDead() || a->IsDisabled() || !a->Is3DLoaded()) continue;
				if (!a->IsHostileToActor(p)) continue;
				RE::NiPoint3 to = Center(a.get()) - start;
				float        d = to.Length();
				if (d > cfg.range || d < 1.0f) continue;
				float ang = std::acos(std::clamp(Norm(to).Dot(fwd), -1.0f, 1.0f)) / DEG;
				if (ang > cfg.coneHalf) continue;
				bool los = false;
				p->HasLineOfSight(a.get(), los);
				if (!los) continue;
				cand.push_back({ h, ang });
			}
		}
		std::sort(cand.begin(), cand.end(), [](const C& x, const C& y) { return x.ang < y.ang; });
		if ((int)cand.size() > cfg.maxTargets) cand.resize(cfg.maxTargets);

		Bullet b;
		b.pos = start;
		b.dir = fwd;
		for (auto& c : cand) b.targets.push_back(c.h);
		if (!cand.empty()) {
			int n = (int)cand.size();
			b.strikesLeft = std::min(cfg.hits + cfg.hitsPerTarget * (n - 1), cfg.maxHits);
		} else if (cfg.noTargetDist > 0.0f) {
			b.noTargetLeft = cfg.noTargetDist / std::max(cfg.speed, 1.0f);
		} else {
			logger::info("no target and NoTargetDist = 0 - nothing launched");
			return;
		}

		auto ptr = p->PlaceObjectAtMe(gStat, false);
		if (!ptr) {
			logger::error("could not place the bullet reference");
			return;
		}
		Place(ptr.get(), start);
		b.ref = ptr->CreateRefHandle();
		gBullets.push_back(std::move(b));
		logger::info("bullet placed: {} targets, {} strikes", cand.size(), gBullets.back().strikesLeft);
	}

	inline void Step(Bullet& b, float dt)
	{
		auto p = RE::PlayerCharacter::GetSingleton();
		auto r = b.ref.get();
		if (!r) { b.done = true; return; }
		b.age += dt;
		b.sinceStrike += dt;
		if (b.age > cfg.life) { Finish(b); return; }

		// nothing in front: straight flight, then a small pop
		if (b.targets.empty()) {
			b.pos += b.dir * (cfg.speed * dt);
			Place(r.get(), b.pos);
			b.noTargetLeft -= dt;
			if (b.noTargetLeft <= 0.0f) { Burst(b.pos, cfg.hitScale); Finish(b); }
			return;
		}

		RE::Actor* tgt = NextTarget(b);
		if (!tgt || b.strikesLeft <= 0) {
			// nobody left to hit (or strikes used up): lodge here with the big burst
			Burst(b.pos, cfg.lastScale);
			Finish(b);
			return;
		}

		float        t = std::clamp(b.sinceStrike / std::max(cfg.turnRamp, 0.01f), 0.0f, 1.0f);
		float        turn = (cfg.turnLow + (cfg.turnHigh - cfg.turnLow) * t) * DEG;
		RE::NiPoint3 tpos = Center(tgt);
		b.dir = RotateToward(b.dir, Norm(tpos - b.pos), turn * dt);
		RE::NiPoint3 np = b.pos + b.dir * (cfg.speed * dt);

		// the target
		if (SegDist(b.pos, np, tpos) <= cfg.hitRadius) {
			b.strikesLeft--;
			b.idx++;
			b.sinceStrike = 0.0f;
			Hurt(tgt, 1.0f);
			bool last = b.strikesLeft <= 0;
			Burst(tpos, last ? cfg.lastScale : cfg.hitScale);
			if (last) { b.pos = tpos; Place(r.get(), b.pos); Finish(b); return; }
			// kick the heading sideways so the return is a wide arc
			RE::NiPoint3 perp = AnyPerp(b.dir);
			perp = RotateAbout(perp, b.dir, Rand(0.0f, 6.2831853f));
			b.dir = Norm(RotateAbout(b.dir, perp, cfg.kick * DEG * Rand(0.6f, 1.4f)));
			np = b.pos + b.dir * (cfg.speed * dt);
		}

		// enemies passed through on the way
		if (cfg.graze > 0.0f) {
			for (auto& h : b.targets) {
				auto a = h.get();
				if (!a || a.get() == tgt || a->IsDead()) continue;
				float& cd = b.grazeCd[a->GetFormID()];
				cd -= dt;
				if (cd > 0.0f) continue;
				RE::NiPoint3 c = Center(a.get());
				if (SegDist(b.pos, np, c) <= cfg.hitRadius) {
					cd = cfg.grazeGap;
					Hurt(a.get(), cfg.graze);
					Burst(c, cfg.hitScale);
				}
			}
		}

		b.pos = np;
		Place(r.get(), b.pos);
	}

	inline void Tick()
	{
		static bool first = true;
		if (first) { first = false; logger::info("per-frame hook is running"); }
		if (auto ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) return;
		float dt = std::min(RE::GetSecondsSinceLastFrame(), 0.05f);
		gClock += dt;
		if (gBullets.empty()) return;
		for (auto& b : gBullets) Step(b, dt);
		gBullets.erase(std::remove_if(gBullets.begin(), gBullets.end(), [](const Bullet& b) { return b.done; }), gBullets.end());
	}

	// ------------------------------------------------------------------ cast detection
	class CastSink : public RE::BSTEventSink<RE::TESSpellCastEvent>
	{
	public:
		static CastSink* Get() { static CastSink s; return &s; }
		RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* e, RE::BSTEventSource<RE::TESSpellCastEvent>*) override
		{
			if (!e || !e->object || !e->object->IsPlayerRef()) return RE::BSEventNotifyControl::kContinue;
			logger::info("player cast event: spell {:08X}", e->spell);
			if (!gPower || !gHand) return RE::BSEventNotifyControl::kContinue;
			bool power = e->spell == gPower->GetFormID();
			bool hand = e->spell == gHand->GetFormID();
			if (!power && !hand) return RE::BSEventNotifyControl::kContinue;
			if (gClock - gLastCast < 0.25f) return RE::BSEventNotifyControl::kContinue;  // guard against a double event
			gLastCast = gClock;
			SKSE::GetTaskInterface()->AddTask([hand]() {
				if (auto p = RE::PlayerCharacter::GetSingleton()) Launch(p, hand);
			});
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	class EffectSink : public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>
	{
	public:
		static EffectSink* Get() { static EffectSink s; return &s; }
		RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* e, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override
		{
			if (!e || !e->caster || !e->caster->IsPlayerRef() || !gTrigger) return RE::BSEventNotifyControl::kContinue;
			if (e->magicEffect != gTrigger->GetFormID()) return RE::BSEventNotifyControl::kContinue;
			logger::info("trigger effect applied");
			if (gClock - gLastCast < 0.25f) return RE::BSEventNotifyControl::kContinue;
			gLastCast = gClock;
			bool hand = false;
			if (auto p = RE::PlayerCharacter::GetSingleton()) {
				for (auto src : { RE::MagicSystem::CastingSource::kLeftHand, RE::MagicSystem::CastingSource::kRightHand }) {
					if (auto c = p->GetMagicCaster(src); c && c->currentSpell == gHand) hand = true;
				}
			}
			SKSE::GetTaskInterface()->AddTask([hand]() {
				if (auto p = RE::PlayerCharacter::GetSingleton()) Launch(p, hand);
			});
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// ------------------------------------------------------------------ per-frame hook (Main::Update call site)
	struct UpdateHook
	{
		static void thunk()
		{
			func();
			Tick();
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void Install()
		{
			SKSE::AllocTrampoline(14);
			auto&                           tr = SKSE::GetTrampoline();
			REL::Relocation<std::uintptr_t> target{ REL::RelocationID(35565, 36564), REL::VariantOffset(0x748, 0xC26, 0x7EE) };
			func = tr.write_call<5>(target.address(), thunk);
		}
	};

	// ------------------------------------------------------------------ learning the spells
	inline void Grant()
	{
		auto p = RE::PlayerCharacter::GetSingleton();
		if (!p || !gPower || !gHand || cfg.grantMode == 0) return;
		if (cfg.grantMode == 1) {
			if (!p->HasSpell(gPower)) p->AddSpell(gPower);
			if (!p->HasSpell(gHand)) p->AddSpell(gHand);
		} else if (cfg.grantMode == 2) {
			auto dh = RE::TESDataHandler::GetSingleton();
			auto add = [&](RE::FormID id, RE::SpellItem* sp) {
				if (p->HasSpell(sp)) return;
				auto bk = dh->LookupForm<RE::TESObjectBOOK>(id, ESP);
				if (bk && p->GetItemCount(bk) == 0) p->AddObjectToContainer(bk, nullptr, 1, nullptr);
			};
			add(ID_TOMEP, gPower);
			add(ID_TOMEH, gHand);
		}
	}

	inline void OnMessage(SKSE::MessagingInterface::Message* m)
	{
		switch (m->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			if (LoadForms()) {
				RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(CastSink::Get());
				RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(EffectSink::Get());
				logger::info("ready");
			}
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			gBullets.clear();
			break;
		case SKSE::MessagingInterface::kNewGame:
		case SKSE::MessagingInterface::kPostLoadGame:
			gBullets.clear();
			Grant();
			break;
		default:
			break;
		}
	}
}

static void SetupLog()
{
	auto path = SKSE::log::log_directory();
	if (!path) return;
	*path /= "SMF_MagicBullet.log"sv;
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
	spdlog::set_default_logger(std::move(log));
	spdlog::set_level(spdlog::level::info);
	spdlog::flush_on(spdlog::level::info);
}

#define MB_EXPORT extern "C" __declspec(dllexport)

// Skyrim SE 1.5.97 (SKSE64 loads plugins through Query + Load)
MB_EXPORT bool SKSEPlugin_Query(const SKSE::QueryInterface*, SKSE::PluginInfo* a_info)
{
	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->name = "SMF_MagicBullet";
	a_info->version = 1;
	return true;
}

MB_EXPORT bool SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();
	MB::LoadConfig();
	MB::UpdateHook::Install();
	SKSE::GetMessagingInterface()->RegisterListener(MB::OnMessage);
	logger::info("SMF_MagicBullet loaded");
	return true;
}
