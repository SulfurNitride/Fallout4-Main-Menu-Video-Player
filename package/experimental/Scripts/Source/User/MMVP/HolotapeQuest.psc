Scriptname MMVP:HolotapeQuest extends Quest

Form Property PlayerHolotape Auto

Event OnQuestInit()
    Actor player = Game.GetPlayer()
    If player.GetItemCount(PlayerHolotape) == 0
        player.AddItem(PlayerHolotape, 1, true)
    EndIf
EndEvent
