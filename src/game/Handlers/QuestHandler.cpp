/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "Log.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "World.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "GossipDef.h"
#include "QuestDef.h"
#include "ObjectAccessor.h"
#include "ScriptMgr.h"
#include "Group.h"

void WorldSession::HandleQuestgiverStatusQueryOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;
    recv_data >> guid;
    uint8 dialogStatus = DIALOG_STATUS_NONE;

    Object* questgiver = _player->GetObjectByTypeMask(guid, TYPEMASK_CREATURE_OR_GAMEOBJECT);
    if (!questgiver)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "Error in CMSG_QUESTGIVER_STATUS_QUERY, called for not found questgiver %s", guid.GetString().c_str());
        return;
    }

    //sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_QUESTGIVER_STATUS_QUERY for %s", guid.GetString().c_str());

    switch (questgiver->GetTypeId())
    {
        case TYPEID_UNIT:
        {
            Creature* cr_questgiver = (Creature*)questgiver;

            if (!cr_questgiver->IsHostileTo(_player))       // not show quest status to enemies
            {
                dialogStatus = sScriptMgr.GetDialogStatus(_player, cr_questgiver);

                if (dialogStatus > 6)
                    dialogStatus = GetDialogStatus(_player, cr_questgiver, DIALOG_STATUS_NONE);
            }
            break;
        }
        case TYPEID_GAMEOBJECT:
        {
            GameObject* go_questgiver = (GameObject*)questgiver;
            dialogStatus = sScriptMgr.GetDialogStatus(_player, go_questgiver);
            if (dialogStatus > 6)
                dialogStatus = GetDialogStatus(_player, go_questgiver, DIALOG_STATUS_NONE);
            break;
        }
        default:
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "QuestGiver called for unexpected type %u", questgiver->GetTypeId());
            break;
    }

    //inform client about status of quest
    _player->PlayerTalkClass->SendQuestGiverStatus(dialogStatus, guid);
}

void WorldSession::HandleQuestgiverHelloOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;
    recv_data >> guid;

    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_QUESTGIVER_HELLO npc: %s", guid.GetString().c_str());

    Creature* pCreature = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);

    if (!pCreature)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleQuestgiverHelloOpcode - %s not found or you can't interact with him.", guid.GetString().c_str());
        return;
    }

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STAT_FEIGN_DEATH))
        GetPlayer()->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);

    // Stop the npc if moving
    if (!pCreature->HasExtraFlag(CREATURE_FLAG_EXTRA_NO_MOVEMENT_PAUSE))
        pCreature->PauseOutOfCombatMovement();

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    if (sScriptMgr.OnGossipHello(_player, pCreature))
        return;

    _player->PrepareGossipMenu(pCreature, pCreature->GetDefaultGossipMenuId());
    _player->SendPreparedGossip(pCreature);
}

void WorldSession::HandleQuestgiverAcceptQuestOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;
    uint32 quest;
    recv_data >> guid >> quest;

    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_QUESTGIVER_ACCEPT_QUEST giver = %s, quest = %u", guid.GetString().c_str(), quest);

    Object* pObject = _player->GetObjectByTypeMask(guid, TYPEMASK_CREATURE_GAMEOBJECT_PLAYER_OR_ITEM);

    // no or incorrect quest giver
    if (!pObject
            || (pObject->GetTypeId() != TYPEID_PLAYER && !pObject->HasQuest(quest))
            || (pObject->GetTypeId() == TYPEID_PLAYER && !((Player*)pObject)->CanShareQuest(quest))
       )
    {
        _player->PlayerTalkClass->CloseGossip();
        _player->ClearDividerGuid();
        return;
    }

    if (!_player->CanInteractWithQuestGiver(pObject))
    {
        _player->PlayerTalkClass->CloseGossip();
        _player->ClearDividerGuid();
        return;
    }

    Quest const* qInfo = sObjectMgr.GetQuestTemplate(quest);
    if (qInfo)
    {
        // prevent cheating
        if (!GetPlayer()->CanTakeQuest(qInfo, true))
        {
            _player->PlayerTalkClass->CloseGossip();
            _player->ClearDividerGuid();
            return;
        }

        if (Player* pPlayer = ObjectAccessor::FindPlayer(_player->GetDividerGuid()))
        {
            pPlayer->SendPushToPartyResponse(_player, QUEST_PARTY_MSG_ACCEPT_QUEST);
            _player->ClearDividerGuid();
        }

        if (_player->CanAddQuest(qInfo, true))
        {
            _player->AddQuest(qInfo, pObject);              // pObject (if it item) can be destroyed at call

            if (qInfo->HasQuestFlag(QUEST_FLAGS_PARTY_ACCEPT))
            {
                if (Group* pGroup = _player->GetGroup())
                {
                    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
                    {
                        Player* pPlayer = itr->getSource();

                        if (!pPlayer || pPlayer == _player || !pPlayer->IsInMap(_player))     // not self and in same map
                            continue;

                        if (pPlayer->CanTakeQuest(qInfo, true))
                        {
                            pPlayer->SetDividerGuid(_player->GetObjectGuid());

                            //need confirmation that any gossip window will close
                            pPlayer->PlayerTalkClass->CloseGossip();

                            _player->SendQuestConfirmAccept(qInfo, pPlayer);
                        }
                    }
                }
            }

            if (_player->CanCompleteQuest(quest))
                _player->CompleteQuest(quest);

            _player->PlayerTalkClass->CloseGossip();

            if (qInfo->GetSrcSpell() > 0)
                _player->CastSpell(_player, qInfo->GetSrcSpell(), true);

            return;
        }
    }

    _player->PlayerTalkClass->CloseGossip();
}

void WorldSession::HandleQuestgiverQueryQuestOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;
    uint32 quest;
    recv_data >> guid >> quest;

    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_QUESTGIVER_QUERY_QUEST npc = %s, quest = %u", guid.GetString().c_str(), quest);

    // Verify that the guid is valid and is a questgiver or involved in the requested quest
    Object* pObject = _player->GetObjectByTypeMask(guid, TYPEMASK_CREATURE_GAMEOBJECT_OR_ITEM);
    if (!pObject || (!pObject->HasQuest(quest) && !pObject->HasInvolvedQuest(quest)))
    {
        _player->PlayerTalkClass->CloseGossip();
        return;
    }

    if (Quest const* pQuest = sObjectMgr.GetQuestTemplate(quest))
        _player->PlayerTalkClass->SendQuestGiverQuestDetails(pQuest, pObject->GetObjectGuid(), true);
}

void WorldSession::HandleQuestQueryOpcode(WorldPacket& recv_data)
{
    uint32 quest;
    recv_data >> quest;
    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_QUEST_QUERY quest = %u", quest);

    Quest const* pQuest = sObjectMgr.GetQuestTemplate(quest);
    if (pQuest)
        _player->PlayerTalkClass->SendQuestQueryResponse(pQuest);
}

void WorldSession::HandleQuestgiverChooseRewardOpcode(WorldPacket& recv_data)
{
    uint32 quest, reward;
    ObjectGuid guid;
    recv_data >> guid >> quest >> reward;

    if (reward >= QUEST_REWARD_CHOICES_COUNT)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "Error in CMSG_QUESTGIVER_CHOOSE_REWARD: player %s (guid %d) tried to get invalid reward (%u) (probably packet hacking)", _player->GetName(), _player->GetGUIDLow(), reward);
        return;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_QUESTGIVER_CHOOSE_REWARD npc = %s, quest = %u, reward = %u", guid.GetString().c_str(), quest, reward);

    WorldObject* pObject = (WorldObject*)_player->GetObjectByTypeMask(guid, TYPEMASK_CREATURE_OR_GAMEOBJECT);
    if (!pObject)
        return;

    if (!pObject->HasInvolvedQuest(quest))
        return;

    if (!GetPlayer()->IsAlive())
    {
        // Some quest can be rewarded while dead (cf q3912 [Meet at the Grave])
        if (Creature* crea = pObject->ToCreature())
        {
            if (!crea->IsVisibleForDead())
                return;
        }
        else
            return;
    }

    Quest const* pQuest = sObjectMgr.GetQuestTemplate(quest);
    if (pQuest)
    {
        if (_player->CanRewardQuest(pQuest, reward, true))
        {
            _player->RewardQuest(pQuest, reward, pObject);

            // Send next quest
            if (Quest const* nextquest = _player->GetNextQuest(guid, pQuest))
                _player->PlayerTalkClass->SendQuestGiverQuestDetails(nextquest, guid, true);
        }
        else
        {
            _player->PlayerTalkClass->SendQuestGiverOfferReward(pQuest, guid, true);
        }
    }
}

void WorldSession::HandleQuestgiverRequestRewardOpcode(WorldPacket& recv_data)
{
    uint32 quest;
    ObjectGuid guid;
    recv_data >> guid >> quest;

    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_QUESTGIVER_REQUEST_REWARD npc = %s, quest = %u", guid.GetString().c_str(), quest);

    Object* pObject = _player->GetObjectByTypeMask(guid, TYPEMASK_CREATURE_OR_GAMEOBJECT);
    if (!pObject || !pObject->HasInvolvedQuest(quest))
        return;

    if (!GetPlayer()->IsAlive())
    {
        // Some quest can be rewarded while dead (cf q3912 [Meet at the Grave])
        if (Creature* crea = pObject->ToCreature())
        {
            if (!crea->IsInvisibleForAlive())
                return;
        }
        else
            return;
    }

    if (_player->CanCompleteQuest(quest))
        _player->CompleteQuest(quest);

    if (_player->GetQuestStatus(quest) != QUEST_STATUS_COMPLETE)
        return;

    if (Quest const* pQuest = sObjectMgr.GetQuestTemplate(quest))
        _player->PlayerTalkClass->SendQuestGiverOfferReward(pQuest, guid, true);
}

void WorldSession::HandleQuestgiverCancel(WorldPacket& /*recv_data*/)
{
    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_QUESTGIVER_CANCEL");

    _player->PlayerTalkClass->CloseGossip();
}

void WorldSession::HandleQuestLogSwapQuest(WorldPacket& recv_data)
{
    uint8 slot1, slot2;
    recv_data >> slot1 >> slot2;

    if (slot1 == slot2 || slot1 >= MAX_QUEST_LOG_SIZE || slot2 >= MAX_QUEST_LOG_SIZE)
        return;

    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_QUESTLOG_SWAP_QUEST slot 1 = %u, slot 2 = %u", slot1, slot2);

    GetPlayer()->SwapQuestSlot(slot1, slot2);
}

void WorldSession::HandleQuestLogRemoveQuest(WorldPacket& recv_data)
{
    uint8 slot;
    recv_data >> slot;

    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_QUESTLOG_REMOVE_QUEST slot = %u", slot);

    _player->RemoveQuestAtSlot(slot);
}

void WorldSession::HandleQuestConfirmAccept(WorldPacket& recv_data)
{
    uint32 quest;
    recv_data >> quest;

    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_QUEST_CONFIRM_ACCEPT quest = %u", quest);

    if (Quest const* pQuest = sObjectMgr.GetQuestTemplate(quest))
    {
        if (!pQuest->HasQuestFlag(QUEST_FLAGS_PARTY_ACCEPT))
            return;

        Player* pOriginalPlayer = ObjectAccessor::FindPlayer(_player->GetDividerGuid());

        if (!pOriginalPlayer)
            return;

        if (pQuest->IsAllowedInRaid())
        {
            if (!_player->IsInSameRaidWith(pOriginalPlayer))
                return;
        }
        else
        {
            if (!_player->IsInSameGroupWith(pOriginalPlayer))
                return;
        }

        if (Quest const* qInfo = sObjectMgr.GetQuestTemplate(quest))
        {
            if (qInfo->HasQuestFlag(QUEST_FLAGS_SHARABLE))
                if (!pOriginalPlayer->IsCurrentQuest(quest))
                    return;
            if (!qInfo->HasQuestFlag(QuestFlags(QUEST_FLAGS_SHARABLE | QUEST_FLAGS_PARTY_ACCEPT)))
                return;
        }

        if (!_player->CanTakeQuest(pQuest, true))
            return;

        if (_player->CanAddQuest(pQuest, true))
            _player->AddQuest(pQuest, nullptr);                // nullptr, this prevent DB script from duplicate running

        _player->ClearDividerGuid();
    }
}

void WorldSession::HandleQuestgiverCompleteQuest(WorldPacket& recv_data)
{
    uint32 quest;
    ObjectGuid guid;
    recv_data >> guid >> quest;

    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_QUESTGIVER_COMPLETE_QUEST npc = %s, quest = %u", guid.GetString().c_str(), quest);

    if (Quest const* pQuest = sObjectMgr.GetQuestTemplate(quest))
    {
        if (_player->GetQuestStatus(quest) != QUEST_STATUS_COMPLETE)
        {
            if (pQuest->IsRepeatable())
                _player->PlayerTalkClass->SendQuestGiverRequestItems(pQuest, guid, _player->CanCompleteRepeatableQuest(pQuest), false);
            else
                _player->PlayerTalkClass->SendQuestGiverRequestItems(pQuest, guid, _player->CanRewardQuest(pQuest, false), false);
        }
        else
            _player->PlayerTalkClass->SendQuestGiverRequestItems(pQuest, guid, _player->CanRewardQuest(pQuest, false), false);
    }
}

void WorldSession::HandleQuestgiverQuestAutoLaunch(WorldPacket& /*recvPacket*/)
{
    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_QUESTGIVER_QUEST_AUTOLAUNCH");
}

void WorldSession::HandlePushQuestToParty(WorldPacket& recvPacket)
{
    uint32 questId;
    recvPacket >> questId;

    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_PUSHQUESTTOPARTY quest = %u", questId);

    if (Quest const* pQuest = sObjectMgr.GetQuestTemplate(questId))
    {
        if (Group* pGroup = _player->GetGroup())
        {
            for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* pPlayer = itr->getSource();

                if (!pPlayer || pPlayer == _player)         // skip self
                    continue;

                _player->SendPushToPartyResponse(pPlayer, QUEST_PARTY_MSG_SHARING_QUEST);

                if (_player->GetDistance(pPlayer) > 10)
                {
                    _player->SendPushToPartyResponse(pPlayer, QUEST_PARTY_MSG_TOO_FAR);
                    continue;
                }

                if (pPlayer->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                {
                    _player->SendPushToPartyResponse(pPlayer, QUEST_PARTY_MSG_FINISH_QUEST);
                    continue;
                }

                if (!pPlayer->SatisfyQuestStatus(pQuest, false))
                {
                    _player->SendPushToPartyResponse(pPlayer, QUEST_PARTY_MSG_HAVE_QUEST);
                    continue;
                }

                if (!pPlayer->CanTakeQuest(pQuest, false))
                {
                    _player->SendPushToPartyResponse(pPlayer, QUEST_PARTY_MSG_CANT_TAKE_QUEST);
                    continue;
                }

                if (!pPlayer->SatisfyQuestLog(false))
                {
                    _player->SendPushToPartyResponse(pPlayer, QUEST_PARTY_MSG_LOG_FULL);
                    continue;
                }

                if (pPlayer->GetDividerGuid())
                {
                    _player->SendPushToPartyResponse(pPlayer, QUEST_PARTY_MSG_BUSY);
                    continue;
                }

                pPlayer->PlayerTalkClass->SendQuestGiverQuestDetails(pQuest, _player->GetObjectGuid(), true);
                pPlayer->SetDividerGuid(_player->GetObjectGuid());
            }
        }
    }
}

void WorldSession::HandleQuestPushResult(WorldPacket& recvPacket)
{
    ObjectGuid guid;
    uint8 msg;
    recvPacket >> guid >> msg;

    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received MSG_QUEST_PUSH_RESULT");

    if (Player* pPlayer = ObjectAccessor::FindPlayer(_player->GetDividerGuid()))
    {
        WorldPacket data(MSG_QUEST_PUSH_RESULT, (8 + 1));
        data << _player->GetObjectGuid();
        data << uint8(msg);                             // enum QuestShareMessages
        pPlayer->GetSession()->SendPacket(&data);
        _player->ClearDividerGuid();
    }
}

/**
* What - if any - kind of explanation mark or question-mark should a quest-giver display for a player
* @param pPlayer - for whom
* @param questgiver - from whom
* @param defstatus - initial set status (usually it will be called with DIALOG_STATUS_NONE) - must not be DIALOG_STATUS_UNDEFINED
*/
uint32 WorldSession::GetDialogStatus(Player* pPlayer, Object* questgiver, uint32 defstatus)
{
    uint32 dialogStatus = defstatus;

    QuestRelationsMapBounds rbounds;                        // QuestRelations (quest-giver)
    QuestRelationsMapBounds irbounds;                       // InvolvedRelations (quest-finisher)

    switch (questgiver->GetTypeId())
    {
        case TYPEID_UNIT:
        {
            rbounds = sObjectMgr.GetCreatureQuestRelationsMapBounds(questgiver->GetEntry());
            irbounds = sObjectMgr.GetCreatureQuestInvolvedRelationsMapBounds(questgiver->GetEntry());
            break;
        }
        case TYPEID_GAMEOBJECT:
        {
            rbounds = sObjectMgr.GetGOQuestRelationsMapBounds(questgiver->GetEntry());
            irbounds = sObjectMgr.GetGOQuestInvolvedRelationsMapBounds(questgiver->GetEntry());
            break;
        }
        default:
            //it's impossible, but check ^)
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "Warning: GetDialogStatus called for unexpected type %u", questgiver->GetTypeId());
            return DIALOG_STATUS_NONE;
    }

    // Check markings for quest-finisher
    for (QuestRelationsMap::const_iterator itr = irbounds.first; itr != irbounds.second; ++itr)
    {
        uint32 dialogStatusNew = DIALOG_STATUS_NONE;
        uint32 quest_id = itr->second;
        Quest const* pQuest = sObjectMgr.GetQuestTemplate(quest_id);

        if (!pQuest || !pQuest->IsActive())
            continue;

        QuestStatus status = pPlayer->GetQuestStatus(quest_id);

        if ((status == QUEST_STATUS_COMPLETE && !pPlayer->GetQuestRewardStatus(quest_id)) ||
                (pQuest->IsAutoComplete() && pPlayer->CanTakeQuest(pQuest, false)))
        {
            if (pQuest->IsAutoComplete() && pQuest->IsRepeatable())
                dialogStatusNew = DIALOG_STATUS_REWARD_REP;
            else
                dialogStatusNew = DIALOG_STATUS_REWARD2;
        }
        else if (status == QUEST_STATUS_INCOMPLETE)
            dialogStatusNew = DIALOG_STATUS_INCOMPLETE;

        if (dialogStatusNew > dialogStatus)
            dialogStatus = dialogStatusNew;
    }

    // check markings for quest-giver
    for (QuestRelationsMap::const_iterator itr = rbounds.first; itr != rbounds.second; ++itr)
    {
        uint32 dialogStatusNew = DIALOG_STATUS_NONE;
        uint32 quest_id = itr->second;
        Quest const* pQuest = sObjectMgr.GetQuestTemplate(quest_id);

        if (!pQuest || !pQuest->IsActive())
            continue;

        QuestStatus status = pPlayer->GetQuestStatus(quest_id);

        if (status == QUEST_STATUS_NONE) // For all other cases the mark is handled either at some place else, or with involved-relations already
        {
            if (pPlayer->CanSeeStartQuest(pQuest))
            {
                if (pPlayer->SatisfyQuestLevel(pQuest, false))
                {
                    int32 lowLevelDiff = sWorld.getConfig(CONFIG_INT32_QUEST_LOW_LEVEL_HIDE_DIFF);
                    if (pQuest->IsAutoComplete() || (pQuest->IsRepeatable() && !pQuest->HasQuestFlag(QUEST_FLAGS_UNK2) && pPlayer->getQuestStatusMap()[quest_id].m_rewarded))
                        dialogStatusNew = DIALOG_STATUS_REWARD_REP;
                    else if (lowLevelDiff < 0 || pPlayer->GetLevel() <= pPlayer->GetQuestLevelForPlayer(pQuest) + uint32(lowLevelDiff))
                        dialogStatusNew = DIALOG_STATUS_AVAILABLE;
                    else
                        dialogStatusNew = DIALOG_STATUS_CHAT;
                }
                else
                    dialogStatusNew = DIALOG_STATUS_UNAVAILABLE;
            }
        }

        if (dialogStatusNew > dialogStatus)
            dialogStatus = dialogStatusNew;
    }

    return dialogStatus;
}
