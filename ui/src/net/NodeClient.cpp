#include "net/NodeClient.h"

namespace tqsdk::ui {

NodeClient::NodeClient(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<tqsdk::ui::NodeClient::State>("tqsdk::ui::NodeClient::State");
    qRegisterMetaType<tqsdk::ui::Quote>("tqsdk::ui::Quote");
    qRegisterMetaType<tqsdk::ui::KlineFrame>("tqsdk::ui::KlineFrame");
    qRegisterMetaType<tqsdk::ui::KlineBar>("tqsdk::ui::KlineBar");
    qRegisterMetaType<tqsdk::ui::Instrument>("tqsdk::ui::Instrument");
    qRegisterMetaType<tqsdk::ui::OptionChain>("tqsdk::ui::OptionChain");
    qRegisterMetaType<tqsdk::ui::Account>("tqsdk::ui::Account");
    qRegisterMetaType<tqsdk::ui::Position>("tqsdk::ui::Position");
    qRegisterMetaType<tqsdk::ui::Order>("tqsdk::ui::Order");
    qRegisterMetaType<tqsdk::ui::Trade>("tqsdk::ui::Trade");
    qRegisterMetaType<tqsdk::ui::OrderAck>("tqsdk::ui::OrderAck");
}

void NodeClient::setState(State state, const QString& message) {
    if (state_ == state && message.isEmpty()) {
        return;
    }
    state_ = state;
    emit stateChanged(state_, message);
}

} // namespace tqsdk::ui
