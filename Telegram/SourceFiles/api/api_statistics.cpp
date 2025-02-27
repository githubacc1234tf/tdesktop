/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_statistics.h"

#include "apiwrap.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_session.h"
#include "statistics/statistics_data_deserialize.h"

namespace Api {
namespace {

constexpr auto kCheckRequestsTimer = 10 * crl::time(1000);

[[nodiscard]] Data::StatisticalGraph StatisticalGraphFromTL(
		const MTPStatsGraph &tl) {
	return tl.match([&](const MTPDstatsGraph &d) {
		using namespace Statistic;
		const auto zoomToken = d.vzoom_token().has_value()
			? qs(*d.vzoom_token()).toUtf8()
			: QByteArray();
		return Data::StatisticalGraph{
			StatisticalChartFromJSON(qs(d.vjson().data().vdata()).toUtf8()),
			zoomToken,
		};
	}, [&](const MTPDstatsGraphAsync &data) {
		return Data::StatisticalGraph{
			.zoomToken = qs(data.vtoken()).toUtf8(),
		};
	}, [&](const MTPDstatsGraphError &data) {
		return Data::StatisticalGraph{ .error = qs(data.verror()) };
	});
}

[[nodiscard]] Data::StatisticalValue StatisticalValueFromTL(
		const MTPStatsAbsValueAndPrev &tl) {
	const auto current = tl.data().vcurrent().v;
	const auto previous = tl.data().vprevious().v;
	return Data::StatisticalValue{
		.value = current,
		.previousValue = previous,
		.growthRatePercentage = previous
			? std::abs((current - previous) / float64(previous) * 100.)
			: 0,
	};
}

[[nodiscard]] Data::ChannelStatistics ChannelStatisticsFromTL(
		const MTPDstats_broadcastStats &data) {
	const auto &tlUnmuted = data.venabled_notifications().data();
	const auto unmuted = (!tlUnmuted.vtotal().v)
		? 0.
		: std::clamp(
			tlUnmuted.vpart().v / tlUnmuted.vtotal().v * 100.,
			0.,
			100.);
	using Recent = MTPPostInteractionCounters;
	auto recentMessages = ranges::views::all(
		data.vrecent_posts_interactions().v
	) | ranges::views::transform([&](const Recent &tl) {
		return tl.match([&](const MTPDpostInteractionCountersStory &data) {
			return Data::StatisticsMessageInteractionInfo{
				.storyId = data.vstory_id().v,
				.viewsCount = data.vviews().v,
				.forwardsCount = data.vforwards().v,
				.reactionsCount = data.vreactions().v,
			};
		}, [&](const MTPDpostInteractionCountersMessage &data) {
			return Data::StatisticsMessageInteractionInfo{
				.messageId = data.vmsg_id().v,
				.viewsCount = data.vviews().v,
				.forwardsCount = data.vforwards().v,
				.reactionsCount = data.vreactions().v,
			};
		});
	}) | ranges::to_vector;

	return {
		.startDate = data.vperiod().data().vmin_date().v,
		.endDate = data.vperiod().data().vmax_date().v,

		.memberCount = StatisticalValueFromTL(data.vfollowers()),
		.meanViewCount = StatisticalValueFromTL(data.vviews_per_post()),
		.meanShareCount = StatisticalValueFromTL(data.vshares_per_post()),
		.meanReactionCount = StatisticalValueFromTL(
			data.vreactions_per_post()),

		.meanStoryViewCount = StatisticalValueFromTL(
			data.vviews_per_story()),
		.meanStoryShareCount = StatisticalValueFromTL(
			data.vshares_per_story()),
		.meanStoryReactionCount = StatisticalValueFromTL(
			data.vreactions_per_story()),

		.enabledNotificationsPercentage = unmuted,

		.memberCountGraph = StatisticalGraphFromTL(
			data.vgrowth_graph()),

		.joinGraph = StatisticalGraphFromTL(
			data.vfollowers_graph()),

		.muteGraph = StatisticalGraphFromTL(
			data.vmute_graph()),

		.viewCountByHourGraph = StatisticalGraphFromTL(
			data.vtop_hours_graph()),

		.viewCountBySourceGraph = StatisticalGraphFromTL(
			data.vviews_by_source_graph()),

		.joinBySourceGraph = StatisticalGraphFromTL(
			data.vnew_followers_by_source_graph()),

		.languageGraph = StatisticalGraphFromTL(
			data.vlanguages_graph()),

		.messageInteractionGraph = StatisticalGraphFromTL(
			data.vinteractions_graph()),

		.instantViewInteractionGraph = StatisticalGraphFromTL(
			data.viv_interactions_graph()),

		.reactionsByEmotionGraph = StatisticalGraphFromTL(
			data.vreactions_by_emotion_graph()),

		.storyInteractionsGraph = StatisticalGraphFromTL(
			data.vstory_interactions_graph()),

		.storyReactionsByEmotionGraph = StatisticalGraphFromTL(
			data.vstory_reactions_by_emotion_graph()),

		.recentMessageInteractions = std::move(recentMessages),
	};
}

[[nodiscard]] Data::SupergroupStatistics SupergroupStatisticsFromTL(
		const MTPDstats_megagroupStats &data) {
	using Senders = MTPStatsGroupTopPoster;
	using Administrators = MTPStatsGroupTopAdmin;
	using Inviters = MTPStatsGroupTopInviter;

	auto topSenders = ranges::views::all(
		data.vtop_posters().v
	) | ranges::views::transform([&](const Senders &tl) {
		return Data::StatisticsMessageSenderInfo{
			.userId = UserId(tl.data().vuser_id().v),
			.sentMessageCount = tl.data().vmessages().v,
			.averageCharacterCount = tl.data().vavg_chars().v,
		};
	}) | ranges::to_vector;
	auto topAdministrators = ranges::views::all(
		data.vtop_admins().v
	) | ranges::views::transform([&](const Administrators &tl) {
		return Data::StatisticsAdministratorActionsInfo{
			.userId = UserId(tl.data().vuser_id().v),
			.deletedMessageCount = tl.data().vdeleted().v,
			.bannedUserCount = tl.data().vkicked().v,
			.restrictedUserCount = tl.data().vbanned().v,
		};
	}) | ranges::to_vector;
	auto topInviters = ranges::views::all(
		data.vtop_inviters().v
	) | ranges::views::transform([&](const Inviters &tl) {
		return Data::StatisticsInviterInfo{
			.userId = UserId(tl.data().vuser_id().v),
			.addedMemberCount = tl.data().vinvitations().v,
		};
	}) | ranges::to_vector;

	return {
		.startDate = data.vperiod().data().vmin_date().v,
		.endDate = data.vperiod().data().vmax_date().v,

		.memberCount = StatisticalValueFromTL(data.vmembers()),
		.messageCount = StatisticalValueFromTL(data.vmessages()),
		.viewerCount = StatisticalValueFromTL(data.vviewers()),
		.senderCount = StatisticalValueFromTL(data.vposters()),

		.memberCountGraph = StatisticalGraphFromTL(
			data.vgrowth_graph()),

		.joinGraph = StatisticalGraphFromTL(
			data.vmembers_graph()),

		.joinBySourceGraph = StatisticalGraphFromTL(
			data.vnew_members_by_source_graph()),

		.languageGraph = StatisticalGraphFromTL(
			data.vlanguages_graph()),

		.messageContentGraph = StatisticalGraphFromTL(
			data.vmessages_graph()),

		.actionGraph = StatisticalGraphFromTL(
			data.vactions_graph()),

		.dayGraph = StatisticalGraphFromTL(
			data.vtop_hours_graph()),

		.weekGraph = StatisticalGraphFromTL(
			data.vweekdays_graph()),

		.topSenders = std::move(topSenders),
		.topAdministrators = std::move(topAdministrators),
		.topInviters = std::move(topInviters),
	};
}

} // namespace

Statistics::Statistics(not_null<ChannelData*> channel)
: StatisticsRequestSender(channel) {
}

StatisticsRequestSender::StatisticsRequestSender(not_null<ChannelData *> channel)
: _channel(channel)
, _api(&_channel->session().api().instance())
, _timer([=] { checkRequests(); }) {
}

StatisticsRequestSender::~StatisticsRequestSender() {
	for (const auto &[dcId, ids] : _requests) {
		for (const auto id : ids) {
			_channel->session().api().unregisterStatsRequest(dcId, id);
		}
	}
}

void StatisticsRequestSender::checkRequests() {
	for (auto i = begin(_requests); i != end(_requests);) {
		for (auto j = begin(i->second); j != end(i->second);) {
			if (_api.pending(*j)) {
				++j;
			} else {
				_channel->session().api().unregisterStatsRequest(
					i->first,
					*j);
				j = i->second.erase(j);
			}
		}
		if (i->second.empty()) {
			i = _requests.erase(i);
		} else {
			++i;
		}
	}
	if (_requests.empty()) {
		_timer.cancel();
	}
}

template <typename Request, typename, typename>
auto StatisticsRequestSender::makeRequest(Request &&request) {
	const auto id = _api.allocateRequestId();
	const auto dcId = _channel->owner().statsDcId(_channel);
	if (dcId) {
		_channel->session().api().registerStatsRequest(dcId, id);
		_requests[dcId].emplace(id);
		if (!_timer.isActive()) {
			_timer.callEach(kCheckRequestsTimer);
		}
	}
	return std::move(_api.request(
		std::forward<Request>(request)
	).toDC(
		dcId ? MTP::ShiftDcId(dcId, MTP::kStatsDcShift) : 0
	).overrideId(id));
}

rpl::producer<rpl::no_value, QString> Statistics::request() {
	return [=](auto consumer) {
		auto lifetime = rpl::lifetime();

		if (!channel()->isMegagroup()) {
			makeRequest(MTPstats_GetBroadcastStats(
				MTP_flags(MTPstats_GetBroadcastStats::Flags(0)),
				channel()->inputChannel
			)).done([=](const MTPstats_BroadcastStats &result) {
				_channelStats = ChannelStatisticsFromTL(result.data());
				consumer.put_done();
			}).fail([=](const MTP::Error &error) {
				consumer.put_error_copy(error.type());
			}).send();
		} else {
			makeRequest(MTPstats_GetMegagroupStats(
				MTP_flags(MTPstats_GetMegagroupStats::Flags(0)),
				channel()->inputChannel
			)).done([=](const MTPstats_MegagroupStats &result) {
				const auto &data = result.data();
				_supergroupStats = SupergroupStatisticsFromTL(data);
				channel()->owner().processUsers(data.vusers());
				consumer.put_done();
			}).fail([=](const MTP::Error &error) {
				consumer.put_error_copy(error.type());
			}).send();
		}

		return lifetime;
	};
}

Statistics::GraphResult Statistics::requestZoom(
		const QString &token,
		float64 x) {
	return [=](auto consumer) {
		auto lifetime = rpl::lifetime();
		const auto wasEmpty = _zoomDeque.empty();
		_zoomDeque.push_back([=] {
			makeRequest(MTPstats_LoadAsyncGraph(
				MTP_flags(x
					? MTPstats_LoadAsyncGraph::Flag::f_x
					: MTPstats_LoadAsyncGraph::Flag(0)),
				MTP_string(token),
				MTP_long(x)
			)).done([=](const MTPStatsGraph &result) {
				consumer.put_next(StatisticalGraphFromTL(result));
				consumer.put_done();
				if (!_zoomDeque.empty()) {
					_zoomDeque.pop_front();
					if (!_zoomDeque.empty()) {
						_zoomDeque.front()();
					}
				}
			}).fail([=](const MTP::Error &error) {
				consumer.put_error_copy(error.type());
			}).send();
		});
		if (wasEmpty) {
			_zoomDeque.front()();
		}

		return lifetime;
	};
}

Data::ChannelStatistics Statistics::channelStats() const {
	return _channelStats;
}

Data::SupergroupStatistics Statistics::supergroupStats() const {
	return _supergroupStats;
}

PublicForwards::PublicForwards(
	not_null<ChannelData*> channel,
	Data::RecentPostId fullId)
: StatisticsRequestSender(channel)
, _fullId(fullId) {
}

void PublicForwards::request(
		const Data::PublicForwardsSlice::OffsetToken &token,
		Fn<void(Data::PublicForwardsSlice)> done) {
	if (!_requestId) {
		if (_fullId.messageId) {
			requestMessage(token, std::move(done));
		} else if (_fullId.storyId) {
			requestStory(token, std::move(done));
		}
	}
}

void PublicForwards::requestMessage(
		const Data::PublicForwardsSlice::OffsetToken &token,
		Fn<void(Data::PublicForwardsSlice)> done) {
	Expects(_fullId.messageId);

	const auto offsetPeer = channel()->owner().peer(token.fullId.peer);
	const auto tlOffsetPeer = offsetPeer
		? offsetPeer->input
		: MTP_inputPeerEmpty();
	constexpr auto kLimit = tl::make_int(100);
	_requestId = makeRequest(MTPstats_GetMessagePublicForwards(
		channel()->inputChannel,
		MTP_int(_fullId.messageId.msg),
		MTP_int(token.rate),
		tlOffsetPeer,
		MTP_int(token.fullId.msg),
		kLimit
	)).done([=, channel = channel()](const MTPmessages_Messages &result) {
		using Messages = QVector<Data::RecentPostId>;
		_requestId = 0;

		auto nextToken = Data::PublicForwardsSlice::OffsetToken();
		const auto process = [&](const MTPVector<MTPMessage> &messages) {
			auto result = Messages();
			for (const auto &message : messages.v) {
				const auto msgId = IdFromMessage(message);
				const auto peerId = PeerFromMessage(message);
				const auto lastDate = DateFromMessage(message);
				if (const auto peer = channel->owner().peerLoaded(peerId)) {
					if (lastDate) {
						channel->owner().addNewMessage(
							message,
							MessageFlags(),
							NewMessageType::Existing);
						nextToken.fullId = { peerId, msgId };
						result.push_back({ .messageId = nextToken.fullId });
					}
				}
			}
			return result;
		};

		auto allLoaded = false;
		auto fullCount = 0;
		auto messages = result.match([&](const MTPDmessages_messages &data) {
			channel->owner().processUsers(data.vusers());
			channel->owner().processChats(data.vchats());
			auto list = process(data.vmessages());
			allLoaded = true;
			fullCount = list.size();
			return list;
		}, [&](const MTPDmessages_messagesSlice &data) {
			channel->owner().processUsers(data.vusers());
			channel->owner().processChats(data.vchats());
			auto list = process(data.vmessages());

			if (const auto nextRate = data.vnext_rate()) {
				const auto rateUpdated = (nextRate->v != token.rate);
				if (rateUpdated) {
					nextToken.rate = nextRate->v;
				} else {
					allLoaded = true;
				}
			}
			fullCount = data.vcount().v;
			return list;
		}, [&](const MTPDmessages_channelMessages &data) {
			channel->owner().processUsers(data.vusers());
			channel->owner().processChats(data.vchats());
			auto list = process(data.vmessages());
			allLoaded = true;
			fullCount = data.vcount().v;
			return list;
		}, [&](const MTPDmessages_messagesNotModified &) {
			allLoaded = true;
			return Messages();
		});

		_lastTotal = std::max(_lastTotal, fullCount);
		done({
			.list = std::move(messages),
			.total = _lastTotal,
			.allLoaded = allLoaded,
			.token = nextToken,
		});
	}).fail([=] {
		_requestId = 0;
	}).send();
}

void PublicForwards::requestStory(
		const Data::PublicForwardsSlice::OffsetToken &token,
		Fn<void(Data::PublicForwardsSlice)> done) {
	Expects(_fullId.storyId);

	constexpr auto kLimit = tl::make_int(100);
	_requestId = makeRequest(MTPstats_GetStoryPublicForwards(
		channel()->input,
		MTP_int(_fullId.storyId.story),
		MTP_string(token.storyOffset),
		kLimit
	)).done([=, channel = channel()](
			const MTPstats_PublicForwards &tlForwards) {
		using Messages = QVector<Data::RecentPostId>;
		_requestId = 0;

		const auto &data = tlForwards.data();

		channel->owner().processUsers(data.vusers());
		channel->owner().processChats(data.vchats());

		const auto nextToken = Data::PublicForwardsSlice::OffsetToken({
			.storyOffset = data.vnext_offset().value_or_empty(),
		});

		const auto allLoaded = nextToken.storyOffset.isEmpty()
			|| (nextToken.storyOffset == token.storyOffset);
		const auto fullCount = data.vcount().v;

		auto recentList = Messages();
		for (const auto &tlForward : data.vforwards().v) {
			tlForward.match([&](const MTPDpublicForwardMessage &data) {
				const auto &message = data.vmessage();
				const auto msgId = IdFromMessage(message);
				const auto peerId = PeerFromMessage(message);
				const auto lastDate = DateFromMessage(message);
				if (const auto peer = channel->owner().peerLoaded(peerId)) {
					if (!lastDate) {
						return;
					}
					channel->owner().addNewMessage(
						message,
						MessageFlags(),
						NewMessageType::Existing);
					recentList.push_back({ .messageId = { peerId, msgId } });
				}
			}, [&](const MTPDpublicForwardStory &data) {
				data.vstory().match([&](const MTPDstoryItem &d) {
					recentList.push_back({
						.storyId = { peerFromMTP(data.vpeer()), d.vid().v }
					});
				}, [](const auto &) {
				});
			});
		}

		_lastTotal = std::max(_lastTotal, fullCount);
		done({
			.list = std::move(recentList),
			.total = _lastTotal,
			.allLoaded = allLoaded,
			.token = nextToken,
		});
	}).fail([=] {
		_requestId = 0;
	}).send();
}

MessageStatistics::MessageStatistics(
	not_null<ChannelData*> channel,
	FullMsgId fullId)
: StatisticsRequestSender(channel)
, _publicForwards(channel, { .messageId = fullId })
, _fullId(fullId) {
}

MessageStatistics::MessageStatistics(
	not_null<ChannelData*> channel,
	FullStoryId storyId)
: StatisticsRequestSender(channel)
, _publicForwards(channel, { .storyId = storyId })
, _storyId(storyId) {
}

Data::PublicForwardsSlice MessageStatistics::firstSlice() const {
	return _firstSlice;
}

void MessageStatistics::request(Fn<void(Data::MessageStatistics)> done) {
	if (channel()->isMegagroup()) {
		return;
	}
	const auto requestFirstPublicForwards = [=](
			const Data::StatisticalGraph &messageGraph,
			const Data::StatisticalGraph &reactionsGraph,
			const Data::StatisticsMessageInteractionInfo &info) {
		const auto callback = [=](Data::PublicForwardsSlice slice) {
			const auto total = slice.total;
			_firstSlice = std::move(slice);
			done({
				.messageInteractionGraph = messageGraph,
				.reactionsByEmotionGraph = reactionsGraph,
				.publicForwards = total,
				.privateForwards = info.forwardsCount - total,
				.views = info.viewsCount,
				.reactions = info.reactionsCount,
			});
		};
		_publicForwards.request({}, callback);
	};

	const auto requestPrivateForwards = [=](
			const Data::StatisticalGraph &messageGraph,
			const Data::StatisticalGraph &reactionsGraph) {
		api().request(MTPchannels_GetMessages(
			channel()->inputChannel,
			MTP_vector<MTPInputMessage>(
				1,
				MTP_inputMessageID(MTP_int(_fullId.msg))))
		).done([=](const MTPmessages_Messages &result) {
			const auto process = [&](const MTPVector<MTPMessage> &messages) {
				const auto &message = messages.v.front();
				return message.match([&](const MTPDmessage &data) {
					auto reactionsCount = 0;
					if (const auto tlReactions = data.vreactions()) {
						const auto &tlCounts = tlReactions->data().vresults();
						for (const auto &tlCount : tlCounts.v) {
							reactionsCount += tlCount.data().vcount().v;
						}
					}
					return Data::StatisticsMessageInteractionInfo{
						.messageId = IdFromMessage(message),
						.viewsCount = data.vviews()
							? data.vviews()->v
							: 0,
						.forwardsCount = data.vforwards()
							? data.vforwards()->v
							: 0,
						.reactionsCount = reactionsCount,
					};
				}, [](const MTPDmessageEmpty &) {
					return Data::StatisticsMessageInteractionInfo();
				}, [](const MTPDmessageService &) {
					return Data::StatisticsMessageInteractionInfo();
				});
			};

			auto info = result.match([&](const MTPDmessages_messages &data) {
				return process(data.vmessages());
			}, [&](const MTPDmessages_messagesSlice &data) {
				return process(data.vmessages());
			}, [&](const MTPDmessages_channelMessages &data) {
				return process(data.vmessages());
			}, [](const MTPDmessages_messagesNotModified &) {
				return Data::StatisticsMessageInteractionInfo();
			});

			requestFirstPublicForwards(
				messageGraph,
				reactionsGraph,
				std::move(info));
		}).fail([=](const MTP::Error &error) {
			requestFirstPublicForwards(messageGraph, reactionsGraph, {});
		}).send();
	};

	const auto requestStoryPrivateForwards = [=](
			const Data::StatisticalGraph &messageGraph,
			const Data::StatisticalGraph &reactionsGraph) {
		api().request(MTPstories_GetStoriesByID(
			channel()->input,
			MTP_vector<MTPint>(1, MTP_int(_storyId.story)))
		).done([=](const MTPstories_Stories &result) {
			const auto &storyItem = result.data().vstories().v.front();
			auto info = storyItem.match([&](const MTPDstoryItem &data) {
				if (!data.vviews()) {
					return Data::StatisticsMessageInteractionInfo();
				}
				const auto &tlViews = data.vviews()->data();
				return Data::StatisticsMessageInteractionInfo{
					.storyId = data.vid().v,
					.viewsCount = tlViews.vviews_count().v,
					.forwardsCount = tlViews.vforwards_count().value_or(0),
					.reactionsCount = tlViews.vreactions_count().value_or(0),
				};
			}, [](const auto &) {
				return Data::StatisticsMessageInteractionInfo();
			});

			requestFirstPublicForwards(
				messageGraph,
				reactionsGraph,
				std::move(info));
		}).fail([=](const MTP::Error &error) {
			requestFirstPublicForwards(messageGraph, reactionsGraph, {});
		}).send();
	};

	if (_storyId) {
		makeRequest(MTPstats_GetStoryStats(
			MTP_flags(MTPstats_GetStoryStats::Flags(0)),
			channel()->input,
			MTP_int(_storyId.story)
		)).done([=](const MTPstats_StoryStats &result) {
			const auto &data = result.data();
			requestStoryPrivateForwards(
				StatisticalGraphFromTL(data.vviews_graph()),
				StatisticalGraphFromTL(data.vreactions_by_emotion_graph()));
		}).fail([=](const MTP::Error &error) {
			requestStoryPrivateForwards({}, {});
		}).send();
	} else {
		makeRequest(MTPstats_GetMessageStats(
			MTP_flags(MTPstats_GetMessageStats::Flags(0)),
			channel()->inputChannel,
			MTP_int(_fullId.msg.bare)
		)).done([=](const MTPstats_MessageStats &result) {
			const auto &data = result.data();
			requestPrivateForwards(
				StatisticalGraphFromTL(data.vviews_graph()),
				StatisticalGraphFromTL(data.vreactions_by_emotion_graph()));
		}).fail([=](const MTP::Error &error) {
			requestPrivateForwards({}, {});
		}).send();
	}
}

Boosts::Boosts(not_null<PeerData*> peer)
: _peer(peer)
, _api(&peer->session().api().instance()) {
}

rpl::producer<rpl::no_value, QString> Boosts::request() {
	return [=](auto consumer) {
		auto lifetime = rpl::lifetime();
		const auto channel = _peer->asChannel();
		if (!channel || channel->isMegagroup()) {
			return lifetime;
		}

		_api.request(MTPpremium_GetBoostsStatus(
			_peer->input
		)).done([=](const MTPpremium_BoostsStatus &result) {
			const auto &data = result.data();
			const auto hasPremium = !!data.vpremium_audience();
			const auto premiumMemberCount = hasPremium
				? std::max(0, int(data.vpremium_audience()->data().vpart().v))
				: 0;
			const auto participantCount = hasPremium
				? std::max(
					int(data.vpremium_audience()->data().vtotal().v),
					premiumMemberCount)
				: 0;
			const auto premiumMemberPercentage = (participantCount > 0)
				? (100. * premiumMemberCount / participantCount)
				: 0;

			const auto slots = data.vmy_boost_slots();
			_boostStatus.overview = Data::BoostsOverview{
				.mine = slots ? int(slots->v.size()) : 0,
				.level = std::max(data.vlevel().v, 0),
				.boostCount = std::max(
					data.vboosts().v,
					data.vcurrent_level_boosts().v),
				.currentLevelBoostCount = data.vcurrent_level_boosts().v,
				.nextLevelBoostCount = data.vnext_level_boosts()
					? data.vnext_level_boosts()->v
					: 0,
				.premiumMemberCount = premiumMemberCount,
				.premiumMemberPercentage = premiumMemberPercentage,
			};
			_boostStatus.link = qs(data.vboost_url());

			if (data.vprepaid_giveaways()) {
				_boostStatus.prepaidGiveaway = ranges::views::all(
					data.vprepaid_giveaways()->v
				) | ranges::views::transform([](const MTPPrepaidGiveaway &r) {
					return Data::BoostPrepaidGiveaway{
						.months = r.data().vmonths().v,
						.id = r.data().vid().v,
						.quantity = r.data().vquantity().v,
						.date = QDateTime::fromSecsSinceEpoch(
							r.data().vdate().v),
					};
				}) | ranges::to_vector;
			}

			using namespace Data;
			requestBoosts({ .gifts = false }, [=](BoostsListSlice &&slice) {
				_boostStatus.firstSliceBoosts = std::move(slice);
				requestBoosts({ .gifts = true }, [=](BoostsListSlice &&s) {
					_boostStatus.firstSliceGifts = std::move(s);
					consumer.put_done();
				});
			});
		}).fail([=](const MTP::Error &error) {
			consumer.put_error_copy(error.type());
		}).send();

		return lifetime;
	};
}

void Boosts::requestBoosts(
		const Data::BoostsListSlice::OffsetToken &token,
		Fn<void(Data::BoostsListSlice)> done) {
	if (_requestId) {
		return;
	}
	constexpr auto kTlFirstSlice = tl::make_int(kFirstSlice);
	constexpr auto kTlLimit = tl::make_int(kLimit);
	const auto gifts = token.gifts;
	_requestId = _api.request(MTPpremium_GetBoostsList(
		gifts
			? MTP_flags(MTPpremium_GetBoostsList::Flag::f_gifts)
			: MTP_flags(0),
		_peer->input,
		MTP_string(token.next),
		token.next.isEmpty() ? kTlFirstSlice : kTlLimit
	)).done([=](const MTPpremium_BoostsList &result) {
		_requestId = 0;

		const auto &data = result.data();
		_peer->owner().processUsers(data.vusers());

		auto list = std::vector<Data::Boost>();
		list.reserve(data.vboosts().v.size());
		constexpr auto kMonthsDivider = int(30 * 86400);
		for (const auto &boost : data.vboosts().v) {
			const auto &data = boost.data();
			const auto path = data.vused_gift_slug()
				? (u"giftcode/"_q + qs(data.vused_gift_slug()->v))
				: QString();
			auto giftCodeLink = !path.isEmpty()
				? Data::GiftCodeLink{
					_peer->session().createInternalLink(path),
					_peer->session().createInternalLinkFull(path),
					qs(data.vused_gift_slug()->v),
				}
				: Data::GiftCodeLink();
			list.push_back({
				data.is_gift(),
				data.is_giveaway(),
				data.is_unclaimed(),
				qs(data.vid()),
				data.vuser_id().value_or_empty(),
				data.vgiveaway_msg_id()
					? FullMsgId{ _peer->id, data.vgiveaway_msg_id()->v }
					: FullMsgId(),
				QDateTime::fromSecsSinceEpoch(data.vdate().v),
				QDateTime::fromSecsSinceEpoch(data.vexpires().v),
				(data.vexpires().v - data.vdate().v) / kMonthsDivider,
				std::move(giftCodeLink),
				data.vmultiplier().value_or_empty(),
			});
		}
		done(Data::BoostsListSlice{
			.list = std::move(list),
			.multipliedTotal = data.vcount().v,
			.allLoaded = (data.vcount().v == data.vboosts().v.size()),
			.token = Data::BoostsListSlice::OffsetToken{
				.next = data.vnext_offset()
					? qs(*data.vnext_offset())
					: QString(),
				.gifts = gifts,
			},
		});
	}).fail([=] {
		_requestId = 0;
	}).send();
}

Data::BoostStatus Boosts::boostStatus() const {
	return _boostStatus;
}

} // namespace Api
