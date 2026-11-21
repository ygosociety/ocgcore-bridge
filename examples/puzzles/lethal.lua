--Puzzle (EDOPro format): two attackers against 3000 LP and a set Trap. Win this turn.
Debug.ReloadFieldBegin(DUEL_ATTACK_FIRST_TURN,5)
Debug.SetPlayerInfo(0,8000,0,0)
Debug.SetPlayerInfo(1,3000,0,0)
--Summoned Skull
Debug.AddCard(70781052,0,0,LOCATION_MZONE,0,POS_FACEUP_ATTACK)
--Gem-Knight Pearl, with two Xyz materials (a card added to an occupied Monster Zone is attached)
Debug.AddCard(71594310,0,0,LOCATION_MZONE,2,POS_FACEUP_ATTACK,true)
Debug.AddCard(69140098,0,0,LOCATION_MZONE,2,POS_FACEUP_ATTACK)
Debug.AddCard(69140098,0,0,LOCATION_MZONE,2,POS_FACEUP_ATTACK)
--Mirror Force, set
Debug.AddCard(44095762,1,1,LOCATION_SZONE,1,POS_FACEDOWN)
Debug.ReloadFieldEnd()
aux.BeginPuzzle()
