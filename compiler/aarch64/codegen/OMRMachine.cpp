/*******************************************************************************
 * Copyright (c) 2018, 2020 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at http://eclipse.org/legal/epl-2.0
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include <stddef.h>
#include <stdint.h>

#include "codegen/ARM64Instruction.hpp"
#include "codegen/BackingStore.hpp"
#include "codegen/CodeGenerator.hpp"
#include "codegen/GenerateInstructions.hpp"
#include "codegen/Machine.hpp"
#include "codegen/Machine_inlines.hpp"
#include "codegen/RealRegister.hpp"
#include "il/Node.hpp"
#include "il/Node_inlines.hpp"
#include "infra/Assert.hpp"

OMR::ARM64::Machine::Machine(TR::CodeGenerator *cg) :
      OMR::Machine(cg)
   {
   self()->initializeRegisterFile();
   }

TR::RealRegister *OMR::ARM64::Machine::findBestFreeRegister(TR_RegisterKinds rk,
                                                            bool considerUnlatched)
   {
   int32_t first;
   int32_t last;

   switch(rk)
      {
      case TR_GPR:
         first = TR::RealRegister::FirstGPR;
         last  = TR::RealRegister::LastAssignableGPR;
         break;
      case TR_FPR:
         first = TR::RealRegister::FirstFPR;
         last  = TR::RealRegister::LastFPR;
         break;
      default:
         TR_ASSERT(false, "Unsupported RegisterKind.");
   }

   uint32_t bestWeightSoFar = 0xffffffff;
   TR::RealRegister *freeRegister = NULL;
   for (int32_t i = first; i <= last; i++)
      {
      if ((_registerFile[i]->getState() == TR::RealRegister::Free ||
           (considerUnlatched &&
            _registerFile[i]->getState() == TR::RealRegister::Unlatched)) &&
          _registerFile[i]->getWeight() < bestWeightSoFar)
         {
         freeRegister    = _registerFile[i];
         bestWeightSoFar = freeRegister->getWeight();
         }
      }
   if (freeRegister != NULL && freeRegister->getState() == TR::RealRegister::Unlatched)
      {
      freeRegister->setAssignedRegister(NULL);
      freeRegister->setState(TR::RealRegister::Free);
      }
   return freeRegister;
   }

TR::RealRegister *OMR::ARM64::Machine::freeBestRegister(TR::Instruction *currentInstruction,
                                                        TR::Register *virtualRegister,
                                                        TR::RealRegister *forced)
   {
   TR::Register *candidates[NUM_ARM64_MAXR];
   TR::Compilation *comp = self()->cg()->comp();
   TR::MemoryReference *tmemref;
   TR_BackingStore *location;
   TR::RealRegister *best;
   TR::Instruction *cursor;
   TR::Node *currentNode = currentInstruction->getNode();
   TR_RegisterKinds rk = (virtualRegister == NULL) ? TR_GPR : virtualRegister->getKind();
   int numCandidates = 0;
   int first, last;
   int32_t dataSize = 0;
   TR::InstOpCode::Mnemonic loadOp;

   if (forced != NULL)
      {
      best = forced;
      candidates[0] = best->getAssignedRegister();
      }
   else
      {
      switch (rk)
         {
         case TR_GPR:
            first = TR::RealRegister::FirstGPR;
            last = TR::RealRegister::LastGPR;
            break;
         case TR_FPR:
            first = TR::RealRegister::FirstFPR;
            last = TR::RealRegister::LastFPR;
            break;
         default:
            TR_ASSERT(false, "Unsupported RegisterKind.");
            break;
         }

      for (int i = first; i <= last; i++)
         {
         TR::RealRegister *realReg = self()->getRealRegister((TR::RealRegister::RegNum)i);
         if (realReg->getState() == TR::RealRegister::Assigned)
            {
            candidates[numCandidates++] = realReg->getAssignedRegister();
            }
         }
      TR_ASSERT(numCandidates != 0, "All registers are blocked");

      cursor = currentInstruction;
      while (numCandidates > 1 &&
             cursor != NULL &&
             cursor->getOpCodeValue() != TR::InstOpCode::label &&
             cursor->getOpCodeValue() != TR::InstOpCode::proc)
         {
         for (int i = 0; i < numCandidates; i++)
            {
            if (cursor->refsRegister(candidates[i]))
               {
               candidates[i] = candidates[--numCandidates];
               }
            }
         cursor = cursor->getPrev();
         }
      best = toRealRegister(candidates[0]->getAssignedRegister());
      }

   TR::Register *registerToSpill = candidates[0];
   TR_Debug *debugObj = self()->cg()->getDebug();
   const bool containsInternalPointer = registerToSpill->containsInternalPointer();
   const bool containsCollectedReference = registerToSpill->containsCollectedReference();

   location = registerToSpill->getBackingStorage();
   switch (rk)
      {
      case TR_GPR:
         if (!comp->getOption(TR_DisableOOL) &&
            (self()->cg()->isOutOfLineColdPath() || self()->cg()->isOutOfLineHotPath()) &&
            registerToSpill->getBackingStorage())
            {
            // reuse the spill slot
            if (debugObj)
               self()->cg()->traceRegisterAssignment("\nOOL: Reuse backing store (%p) for %s inside OOL\n",
                                          location, debugObj->getName(registerToSpill));
            }
         else if (!containsInternalPointer)
            {
            location = self()->cg()->allocateSpill(TR::Compiler->om.sizeofReferenceAddress(), registerToSpill->containsCollectedReference(), NULL);

            if (debugObj)
               self()->cg()->traceRegisterAssignment("\nSpilling %s to (%p)\n",debugObj->getName(registerToSpill), location);
            }
         else
            {
            location = self()->cg()->allocateInternalPointerSpill(registerToSpill->getPinningArrayPointer());
            if (debugObj)
               self()->cg()->traceRegisterAssignment("\nSpilling internal pointer %s to (%p)\n", debugObj->getName(registerToSpill), location);
            }
         break;
      case TR_FPR:
         if (!comp->getOption(TR_DisableOOL) &&
            (self()->cg()->isOutOfLineColdPath() || self()->cg()->isOutOfLineHotPath()) &&
            registerToSpill->getBackingStorage())
            {
            // reuse the spill slot
            if (debugObj)
               self()->cg()->traceRegisterAssignment("\nOOL: Reuse backing store (%p) for %s inside OOL\n",
                                         location, debugObj->getName(registerToSpill));
            }
         else
            {
            location = self()->cg()->allocateSpill(8, false, NULL);
            if (debugObj)
               self()->cg()->traceRegisterAssignment("\nSpilling FPR %s to (%p)\n", debugObj->getName(registerToSpill), location);
            }
         break;
      default:
         TR_ASSERT(false, "Unsupported RegisterKind.");
         break;
      }

   registerToSpill->setBackingStorage(location);

   tmemref = new (self()->cg()->trHeapMemory()) TR::MemoryReference(currentNode, location->getSymbolReference(), self()->cg());

   if (!comp->getOption(TR_DisableOOL))
      {
      if (!self()->cg()->isOutOfLineColdPath())
         {
         // the spilledRegisterList contains all registers that are spilled before entering
         // the OOL cold path, post dependencies will be generated using this list
         self()->cg()->getSpilledRegisterList()->push_front(registerToSpill);

         // OOL cold path: depth = 3, hot path: depth = 2,  main line: depth = 1
         // if the spill is outside of the OOL cold/hot path, we need to protect the spill slot
         // if we reverse spill this register inside the OOL cold/hot path
         if (!self()->cg()->isOutOfLineHotPath())
            {// main line
            location->setMaxSpillDepth(1);
            }
         else
            {
            // hot path
            // do not overwrite main line spill depth
            if (location->getMaxSpillDepth() != 1)
               {
               location->setMaxSpillDepth(2);
               }
            }
         if (debugObj)
            self()->cg()->traceRegisterAssignment("OOL: adding %s to the spilledRegisterList, maxSpillDepth = %d ",
                                          debugObj->getName(registerToSpill), location->getMaxSpillDepth());
         }
      else
         {
         // do not overwrite mainline and hot path spill depth
         // if this spill is inside OOL cold path, we do not need to protecting the spill slot
         // because the post condition at OOL entry does not expect this register to be spilled
         if (location->getMaxSpillDepth() != 1 &&
             location->getMaxSpillDepth() != 2 )
            {
            location->setMaxSpillDepth(3);
            self()->cg()->traceRegisterAssignment("OOL: In OOL cold path, spilling %s not adding to spilledRegisterList", registerToSpill->getRegisterName(self()->cg()->comp()));
            }
         }
      }

   if (self()->cg()->comp()->getOption(TR_TraceCG))
      {
      diagnostic("\n\tspilling %s (%s)",
                  best->getAssignedRegister()->getRegisterName(self()->cg()->comp()),
                  best->getRegisterName(self()->cg()->comp()));
      }

   switch (rk)
      {
      case TR_GPR:
         loadOp = TR::InstOpCode::ldrimmx;
         break;
      case TR_FPR:
         loadOp = TR::InstOpCode::vldrimmd;
         break;
      default:
         TR_ASSERT(false, "Unsupported RegisterKind.");
         break;
      }
   generateTrg1MemInstruction(self()->cg(), loadOp, currentNode, best, tmemref, currentInstruction);

   self()->cg()->traceRegFreed(registerToSpill, best);

   best->setAssignedRegister(NULL);
   best->setState(TR::RealRegister::Free);
   registerToSpill->setAssignedRegister(NULL);
   return best;
   }

TR::RealRegister *OMR::ARM64::Machine::reverseSpillState(TR::Instruction *currentInstruction,
                                                     TR::Register *spilledRegister,
                                                     TR::RealRegister *targetRegister)
   {
   TR::Compilation *comp = self()->cg()->comp();
   TR::MemoryReference *tmemref;
   TR::RealRegister *sameReg;
   TR_BackingStore *location = spilledRegister->getBackingStorage();
   TR::Node *currentNode = currentInstruction->getNode();
   TR_RegisterKinds rk = spilledRegister->getKind();
   TR_Debug *debugObj = self()->cg()->getDebug();
   int32_t dataSize = 0;
   TR::InstOpCode::Mnemonic storeOp;

   if (targetRegister == NULL)
      {
      targetRegister = self()->findBestFreeRegister(rk);
      if (targetRegister == NULL)
         {
         targetRegister = self()->freeBestRegister(currentInstruction, spilledRegister, NULL);
         }
      targetRegister->setState(TR::RealRegister::Assigned);
      }

   if (self()->cg()->isOutOfLineColdPath())
      {
      // the future and total use count might not always reflect register spill state
      // for example a new register assignment in the hot path would cause FC != TC
      // in this case, assign a new register and return
      if (!location)
         {
         if (debugObj)
            self()->cg()->traceRegisterAssignment("OOL: Not generating reverse spill for (%s)\n", debugObj->getName(spilledRegister));
         return targetRegister;
         }
      }

   if (comp->getOption(TR_TraceCG))
      {
      diagnostic("\n\tre-assigning spilled %s to %s",
                  spilledRegister->getRegisterName(comp),
                  targetRegister->getRegisterName(comp));
      }

   tmemref = new (self()->cg()->trHeapMemory()) TR::MemoryReference(currentNode, location->getSymbolReference(), self()->cg());

   if (comp->getOption(TR_DisableOOL))
      {
      switch (rk)
         {
         case TR_GPR:
            dataSize = TR::Compiler->om.sizeofReferenceAddress();
            storeOp = TR::InstOpCode::strimmx;
            break;
         case TR_FPR:
            dataSize = 8;
            storeOp = TR::InstOpCode::vstrimmd;
            break;
         default:
            TR_ASSERT(false, "Unsupported RegisterKind.");
            break;
         }
         self()->cg()->freeSpill(location, dataSize, 0);
         generateMemSrc1Instruction(self()->cg(), storeOp, currentNode, tmemref, targetRegister, currentInstruction);
      }
   else
      {
      switch (rk)
         {
         case TR_GPR:
            dataSize = TR::Compiler->om.sizeofReferenceAddress();
            break;
         case TR_FPR:
            dataSize = 8;
            break;
         default:
            TR_ASSERT(false, "Unsupported RegisterKind.");
            break;
         }
      if (self()->cg()->isOutOfLineColdPath())
         {
         bool isOOLentryReverseSpill = false;
         if (currentInstruction->isLabel())
            {
            if (((TR::ARM64LabelInstruction*)currentInstruction)->getLabelSymbol()->isStartOfColdInstructionStream())
               {
               // indicates that we are at OOL entry point post conditions. Since
               // we are now exiting the OOL cold path (going reverse order)
               // and we called reverseSpillState(), the main line path
               // expects the Virt reg to be assigned to a real register
               // we can now safely unlock the protected backing storage
               // This prevents locking backing storage for future OOL blocks
               isOOLentryReverseSpill = true;
               }
            }
         // OOL: only free the spill slot if the register was spilled in the same or less dominant path
         // ex: spilled in cold path, reverse spill in hot path or main line
         // we have to spill this register again when we reach OOL entry point due to post
         // conditions. We want to guarantee that the same spill slot will be protected and reused.
         // maxSpillDepth: 3:cold path, 2:hot path, 1:main line
         // Also free the spill if maxSpillDepth==0, which will be the case if the reverse spill also occured on the hot path.
         // If the reverse spill occured on both paths then this is the last chance we have to free the spill slot.
         if (location->getMaxSpillDepth() == 3 || location->getMaxSpillDepth() == 0 || isOOLentryReverseSpill)
            {
            if (location->getMaxSpillDepth() != 0)
               location->setMaxSpillDepth(0);
            else if (debugObj)
               self()->cg()->traceRegisterAssignment("\nOOL: reverse spill %s in less dominant path (%d / 3), reverse spill on both paths indicated, free spill slot (%p)\n",
                                             debugObj->getName(spilledRegister), location->getMaxSpillDepth(), location);
            self()->cg()->freeSpill(location, dataSize, 0);

            if (!self()->cg()->isFreeSpillListLocked())
               {
               spilledRegister->setBackingStorage(NULL);
               }
            }
         else
            {
            if (debugObj)
               self()->cg()->traceRegisterAssignment("\nOOL: reverse spill %s in less dominant path (%d / 3), protect spill slot (%p)\n",
                                             debugObj->getName(spilledRegister), location->getMaxSpillDepth(), location);
            }
         }
      else if (self()->cg()->isOutOfLineHotPath())
         {
         // the spilledRegisterList contains all registers that are spilled before entering
         // the OOL path (in backwards RA). Post dependencies will be generated using this list.
         // Any registers reverse spilled before entering OOL should be removed from the spilled list
         if (debugObj)
            self()->cg()->traceRegisterAssignment("\nOOL: removing %s from the spilledRegisterList\n", debugObj->getName(spilledRegister));
         self()->cg()->getSpilledRegisterList()->remove(spilledRegister);

         // Reset maxSpillDepth here so that in the cold path we know to free the spill
         // and so that the spill is not included in future GC points in the hot path while it is protected
         location->setMaxSpillDepth(0);
         if (location->getMaxSpillDepth() == 2)
            {
            self()->cg()->freeSpill(location, dataSize, 0);
            if (!self()->cg()->isFreeSpillListLocked())
               {
               spilledRegister->setBackingStorage(NULL);
               }
            }
         else
            {
            if (debugObj)
               self()->cg()->traceRegisterAssignment("\nOOL: reverse spilling %s in less dominant path (%d / 2), protect spill slot (%p)\n",
                                             debugObj->getName(spilledRegister), location->getMaxSpillDepth(), location);
            }
         }
      else // main line
         {
         if (debugObj)
            self()->cg()->traceRegisterAssignment("\nOOL: removing %s from the spilledRegisterList)\n", debugObj->getName(spilledRegister));
         self()->cg()->getSpilledRegisterList()->remove(spilledRegister);
         location->setMaxSpillDepth(0);
         self()->cg()->freeSpill(location, dataSize, 0);

         if (!self()->cg()->isFreeSpillListLocked())
            {
            spilledRegister->setBackingStorage(NULL);
            }
         }
      switch (rk)
         {
         case TR_GPR:
            storeOp = TR::InstOpCode::strimmx;
            break;
         case TR_FPR:
            storeOp = TR::InstOpCode::vstrimmd;
            break;
         default:
            TR_ASSERT(false, "Unsupported RegisterKind.");
            break;
         }
         generateMemSrc1Instruction(self()->cg(), storeOp, currentNode, tmemref, targetRegister, currentInstruction);
      }
   return targetRegister;
   }

TR::RealRegister *OMR::ARM64::Machine::assignOneRegister(TR::Instruction *currentInstruction,
                                                         TR::Register *virtualRegister)
   {
   TR_RegisterKinds rk = virtualRegister->getKind();
   TR::RealRegister *assignedRegister = virtualRegister->getAssignedRealRegister();
   TR::CodeGenerator *cg = self()->cg();
   TR::Compilation *comp = cg->comp();

   if (assignedRegister == NULL)
      {
      cg->clearRegisterAssignmentFlags();
      cg->setRegisterAssignmentFlag(TR_NormalAssignment);

      if (virtualRegister->getTotalUseCount() != virtualRegister->getFutureUseCount())
         {
         cg->setRegisterAssignmentFlag(TR_RegisterReloaded);
         assignedRegister = self()->reverseSpillState(currentInstruction, virtualRegister, NULL);
         }
      else
         {
         assignedRegister = self()->findBestFreeRegister(rk, true);
         if (assignedRegister == NULL)
            {
            cg->setRegisterAssignmentFlag(TR_RegisterSpilled);
            assignedRegister = self()->freeBestRegister(currentInstruction, virtualRegister, NULL);
            }
         if (!comp->getOption(TR_DisableOOL) && cg->isOutOfLineColdPath())
            {
            cg->getFirstTimeLiveOOLRegisterList()->push_front(virtualRegister);
            }
         }

      virtualRegister->setAssignedRegister(assignedRegister);
      assignedRegister->setAssignedRegister(virtualRegister);
      assignedRegister->setState(TR::RealRegister::Assigned);
      cg->traceRegAssigned(virtualRegister, assignedRegister);
      }
   else
      {
      TR_Debug *debugObj = cg->getDebug();
      auto registerName = (debugObj != NULL) ? debugObj->getName(assignedRegister) : "NULL";

      TR_ASSERT_FATAL(assignedRegister->getAssignedRegister(), "assignedRegister(%s) does not have assigned virtual register", registerName);
      }
   // Do bookkeeping register use count
   decFutureUseCountAndUnlatch(currentInstruction, virtualRegister);

   return assignedRegister;
   }

/* generate instruction for register copy */
static void registerCopy(TR::Instruction *precedingInstruction,
                         TR_RegisterKinds rk,
                         TR::RealRegister *targetReg,
                         TR::RealRegister *sourceReg,
                         TR::CodeGenerator *cg)
   {
   TR::Node *node = precedingInstruction->getNode();
   TR::RealRegister *zeroReg;
   switch (rk)
      {
      case TR_GPR:
         zeroReg = cg->machine()->getRealRegister(TR::RealRegister::xzr);
         generateTrg1Src2Instruction(cg, TR::InstOpCode::orrx, node, targetReg, zeroReg, sourceReg, precedingInstruction); /* mov (register) */
         break;
      case TR_FPR:
         generateTrg1Src1Instruction(cg, TR::InstOpCode::fmovd, node, targetReg, sourceReg, precedingInstruction);
         break;
      default:
         TR_ASSERT(false, "Unsupported RegisterKind.");
         break;
      }
   }

/* generate instructions for register exchange */
static void registerExchange(TR::Instruction *precedingInstruction,
                             TR_RegisterKinds rk,
                             TR::RealRegister *targetReg,
                             TR::RealRegister *sourceReg,
                             TR::RealRegister *middleReg,
                             TR::CodeGenerator *cg)
   {
   // middleReg is not used if rk==TR_GPR.

   TR::Node *node = precedingInstruction->getNode();
   if (rk == TR_GPR)
      {
      generateTrg1Src2Instruction(cg, TR::InstOpCode::eorx, node, targetReg, targetReg, sourceReg, precedingInstruction);
      generateTrg1Src2Instruction(cg, TR::InstOpCode::eorx, node, sourceReg, targetReg, sourceReg, precedingInstruction);
      generateTrg1Src2Instruction(cg, TR::InstOpCode::eorx, node, targetReg, targetReg, sourceReg, precedingInstruction);
      }
   else
      {
      registerCopy(precedingInstruction, rk, targetReg, middleReg, cg);
      registerCopy(precedingInstruction, rk, sourceReg, targetReg, cg);
      registerCopy(precedingInstruction, rk, middleReg, sourceReg, cg);
      }
   }

void OMR::ARM64::Machine::coerceRegisterAssignment(TR::Instruction *currentInstruction,
                                                   TR::Register *virtualRegister,
                                                   TR::RealRegister::RegNum registerNumber)
   {
   TR::Compilation *comp = self()->cg()->comp();
   TR::RealRegister *targetRegister = _registerFile[registerNumber];
   TR::RealRegister *realReg = virtualRegister->getAssignedRealRegister();
   TR::RealRegister *currentAssignedRegister = realReg ? toRealRegister(realReg) : NULL;
   TR_RegisterKinds rk = virtualRegister->getKind();

   if (comp->getOption(TR_TraceCG))
      {
      if (currentAssignedRegister)
         diagnostic("\n\tcoercing %s from %s to %s",
                     virtualRegister->getRegisterName(comp),
                     currentAssignedRegister->getRegisterName(comp),
                     targetRegister->getRegisterName(comp));
      else
         diagnostic("\n\tcoercing %s to %s",
                     virtualRegister->getRegisterName(comp),
                     targetRegister->getRegisterName(comp));
      }

   if (currentAssignedRegister == targetRegister)
       return;

   if (  targetRegister->getState() == TR::RealRegister::Free
      || targetRegister->getState() == TR::RealRegister::Unlatched)
      {
#ifdef DEBUG
      if (comp->getOption(TR_TraceCG))
         diagnostic(", which is free");
#endif
      if (currentAssignedRegister == NULL)
         {
         if (virtualRegister->getTotalUseCount() != virtualRegister->getFutureUseCount())
            {
            self()->cg()->setRegisterAssignmentFlag(TR_RegisterReloaded);
            self()->reverseSpillState(currentInstruction, virtualRegister, targetRegister);
            }
         else
            {
            if (!comp->getOption(TR_DisableOOL) && self()->cg()->isOutOfLineColdPath())
               {
               self()->cg()->getFirstTimeLiveOOLRegisterList()->push_front(virtualRegister);
               }
            }
         }
      else
         {
         registerCopy(currentInstruction, rk, currentAssignedRegister, targetRegister, self()->cg());
         currentAssignedRegister->setState(TR::RealRegister::Free);
         currentAssignedRegister->setAssignedRegister(NULL);
         }
      }
   else
      {
      TR::RealRegister *spareReg = NULL;
      TR::Register *currentTargetVirtual = targetRegister->getAssignedRegister();

      bool needTemp = (rk == TR_FPR); // xor is unavailable for register exchange

      if (targetRegister->getState() == TR::RealRegister::Blocked)
         {
#ifdef DEBUG
         if (comp->getOption(TR_TraceCG))
            diagnostic(", which is blocked and assigned to %s",
                       currentTargetVirtual->getRegisterName(comp));
#endif
         if (!currentAssignedRegister || needTemp)
            {
            spareReg = self()->findBestFreeRegister(rk);
            self()->cg()->setRegisterAssignmentFlag(TR_IndirectCoercion);
            if (spareReg == NULL)
               {
               self()->cg()->setRegisterAssignmentFlag(TR_RegisterSpilled);
               virtualRegister->block();
               spareReg = self()->freeBestRegister(currentInstruction, currentTargetVirtual);
               virtualRegister->unblock();
               }
            }

         if (currentAssignedRegister)
            {
            self()->cg()->traceRegAssigned(currentTargetVirtual, currentAssignedRegister);
            registerExchange(currentInstruction, rk, targetRegister, currentAssignedRegister, spareReg, self()->cg());
            currentAssignedRegister->setState(TR::RealRegister::Blocked);
            currentAssignedRegister->setAssignedRegister(currentTargetVirtual);
            currentTargetVirtual->setAssignedRegister(currentAssignedRegister);
            // For Non-GPR, spareReg remains FREE.
            }
         else
            {
            self()->cg()->traceRegAssigned(currentTargetVirtual, spareReg);
            registerCopy(currentInstruction, rk, targetRegister, spareReg, self()->cg());
            spareReg->setState(TR::RealRegister::Blocked);
            currentTargetVirtual->setAssignedRegister(spareReg);
            spareReg->setAssignedRegister(currentTargetVirtual);
            // spareReg is assigned.

            if (virtualRegister->getTotalUseCount() != virtualRegister->getFutureUseCount())
               {
               self()->cg()->setRegisterAssignmentFlag(TR_RegisterReloaded);
               self()->reverseSpillState(currentInstruction, virtualRegister, targetRegister);
               }
            else
               {
               if (!comp->getOption(TR_DisableOOL) && self()->cg()->isOutOfLineColdPath())
                  {
                  self()->cg()->getFirstTimeLiveOOLRegisterList()->push_front(virtualRegister);
                  }
               }
            }
         }
      else if (targetRegister->getState() == TR::RealRegister::Assigned)
         {
#ifdef DEBUG
         if (comp->getOption(TR_TraceCG))
            diagnostic(", which is assigned to %s",
                       currentTargetVirtual->getRegisterName(comp));
#endif
         if (!currentAssignedRegister || needTemp)
            spareReg = self()->findBestFreeRegister(rk);

         self()->cg()->setRegisterAssignmentFlag(TR_IndirectCoercion);
         if (currentAssignedRegister)
            {
            if (!needTemp || (spareReg != NULL))
               {
               self()->cg()->traceRegAssigned(currentTargetVirtual, currentAssignedRegister);
               registerExchange(currentInstruction, rk, targetRegister,
                                currentAssignedRegister, spareReg, self()->cg());
               currentAssignedRegister->setState(TR::RealRegister::Assigned);
               currentAssignedRegister->setAssignedRegister(currentTargetVirtual);
               currentTargetVirtual->setAssignedRegister(currentAssignedRegister);
               // spareReg is still FREE.
               }
            else
               {
               self()->freeBestRegister(currentInstruction, currentTargetVirtual, targetRegister);
               self()->cg()->traceRegAssigned(currentTargetVirtual, currentAssignedRegister);
               self()->cg()->setRegisterAssignmentFlag(TR_RegisterSpilled);
               registerCopy(currentInstruction, rk, currentAssignedRegister, targetRegister, self()->cg());
               currentAssignedRegister->setState(TR::RealRegister::Free);
               currentAssignedRegister->setAssignedRegister(NULL);
               }
            }
         else
            {
            if (spareReg == NULL)
               {
               self()->cg()->setRegisterAssignmentFlag(TR_RegisterSpilled);
               self()->freeBestRegister(currentInstruction, currentTargetVirtual, targetRegister);
               }
            else
               {
               self()->cg()->traceRegAssigned(currentTargetVirtual, spareReg);
               registerCopy(currentInstruction, rk, targetRegister, spareReg, self()->cg());
               spareReg->setState(TR::RealRegister::Assigned);
               spareReg->setAssignedRegister(currentTargetVirtual);
               currentTargetVirtual->setAssignedRegister(spareReg);
               // spareReg is assigned.
               }

            if (virtualRegister->getTotalUseCount() != virtualRegister->getFutureUseCount())
               {
               self()->cg()->setRegisterAssignmentFlag(TR_RegisterReloaded);
               self()->reverseSpillState(currentInstruction, virtualRegister, targetRegister);
               }
            else
               {
               if (!comp->getOption(TR_DisableOOL) && self()->cg()->isOutOfLineColdPath())
                  {
                  self()->cg()->getFirstTimeLiveOOLRegisterList()->push_front(virtualRegister);
                  }
               }
            }
         self()->cg()->resetRegisterAssignmentFlag(TR_IndirectCoercion);
         }
      else
         {
#ifdef DEBUG
         if (comp->getOption(TR_TraceCG))
            diagnostic(", which is in an unknown state %d", targetRegister->getState());
#endif
         }
      }

   targetRegister->setState(TR::RealRegister::Assigned);
   targetRegister->setAssignedRegister(virtualRegister);
   virtualRegister->setAssignedRegister(targetRegister);
   self()->cg()->traceRegAssigned(virtualRegister, targetRegister);
   }

void OMR::ARM64::Machine::initializeRegisterFile()
   {
   _registerFile[TR::RealRegister::NoReg] = NULL;
   _registerFile[TR::RealRegister::SpilledReg] = NULL;

   _registerFile[TR::RealRegister::x0] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x0,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x1] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x1,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x2] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x2,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x3] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x3,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x4] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x4,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x5] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x5,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x6] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x6,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x7] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x7,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x8] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x8,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x9] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x9,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x10] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x10,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x11] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x11,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x12] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x12,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x13] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x13,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x14] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x14,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x15] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x15,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x16] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x16,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x17] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x17,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x18] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x18,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x19] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x19,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x20] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x20,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x21] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x21,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x22] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x22,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x23] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x23,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x24] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x24,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x25] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x25,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x26] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x26,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x27] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x27,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x28] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x28,
                                                 self()->cg());

   _registerFile[TR::RealRegister::x29] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::x29,
                                                 self()->cg());

   /* x30 is used as LR on ARM64 */
   _registerFile[TR::RealRegister::lr] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::lr,
                                                 self()->cg());

   /* SP */
   _registerFile[TR::RealRegister::sp] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::sp,
                                                 self()->cg());

   /* XZR */
   _registerFile[TR::RealRegister::xzr] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_GPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::xzr,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v0] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v0,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v1] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v1,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v2] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v2,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v3] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v3,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v4] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v4,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v5] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v5,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v6] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v6,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v7] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v7,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v8] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v8,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v9] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v9,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v10] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v10,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v11] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v11,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v12] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v12,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v13] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v13,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v14] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v14,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v15] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v15,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v16] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v16,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v17] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v17,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v18] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v18,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v19] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v19,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v20] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v20,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v21] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v21,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v22] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v22,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v23] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v23,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v24] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v24,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v25] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v25,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v26] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v26,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v27] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v27,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v28] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v28,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v29] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v29,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v30] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v30,
                                                 self()->cg());

   _registerFile[TR::RealRegister::v31] = new (self()->cg()->trHeapMemory()) TR::RealRegister(TR_FPR,
                                                 0,
                                                 TR::RealRegister::Free,
                                                 TR::RealRegister::v31,
                                                 self()->cg());
   }

void
OMR::ARM64::Machine::takeRegisterStateSnapShot()
   {
   int32_t i;
   for (i = TR::RealRegister::FirstGPR; i < TR::RealRegister::NumRegisters - 1; i++)
      {
      _registerStatesSnapShot[i] = _registerFile[i]->getState();
      _assignedRegisterSnapShot[i] = _registerFile[i]->getAssignedRegister();
      _registerFlagsSnapShot[i] = _registerFile[i]->getFlags();
      }
   }

void
OMR::ARM64::Machine::restoreRegisterStateFromSnapShot()
   {
   int32_t i;
   for (i = TR::RealRegister::FirstGPR; i < TR::RealRegister::NumRegisters - 1; i++) // Skipping SpilledReg
      {
      _registerFile[i]->setFlags(_registerFlagsSnapShot[i]);
      _registerFile[i]->setState(_registerStatesSnapShot[i]);
      if (_registerFile[i]->getState() == TR::RealRegister::Free)
         {
         if (_registerFile[i]->getAssignedRegister() != NULL)
            {
            // clear the Virt -> Real reg assignment if we restored the Real reg state to FREE
            _registerFile[i]->getAssignedRegister()->setAssignedRegister(NULL);
            }
         }
      else if (_registerFile[i]->getState() == TR::RealRegister::Assigned)
         {
         if (_registerFile[i]->getAssignedRegister() != NULL &&
             _registerFile[i]->getAssignedRegister() != _assignedRegisterSnapShot[i])
            {
            // If the virtual register associated with the _registerFile[i] is not assigned to the current real register
            // it must have been updated by prior _registerFile.... Do NOT Clear.
            //   Ex:
            //     RegFile starts as:
            //       _registerFile[12] -> GPR_3555
            //       _registerFile[15] -> GPR_3545
            //     SnapShot:
            //       _registerFile[12] -> GPR_3545
            //       _registerFile[15] -> GPR_3562
            //  When we handled _registerFile[12], we would have updated the assignment of GPR_3545 (currently to GPR15) to GPR12.
            //  When we subsequently handle _registerFile[15], we cannot blindly reset GPR_3545's assigned register to NULL,
            //  as that will incorrectly break the assignment to GPR12.
            if (_registerFile[i]->getAssignedRegister()->getAssignedRegister() == _registerFile[i])
               {
               // clear the Virt -> Real reg assignment for any newly assigned virtual register (due to spills) in the hot path
               _registerFile[i]->getAssignedRegister()->setAssignedRegister(NULL);
               }
            }
         }
      _registerFile[i]->setAssignedRegister(_assignedRegisterSnapShot[i]);
      // make sure to double link virt - real reg if assigned
      if (_registerFile[i]->getState() == TR::RealRegister::Assigned)
         {
         _registerFile[i]->getAssignedRegister()->setAssignedRegister(_registerFile[i]);
         }
      // Don't restore registers that died after the snapshot was taken since they are guaranteed to not be used in the outlined path
      if (_registerFile[i]->getState() == TR::RealRegister::Assigned &&
          _registerFile[i]->getAssignedRegister()->getFutureUseCount() == 0)
         {
         _registerFile[i]->setState(TR::RealRegister::Free);
         _registerFile[i]->getAssignedRegister()->setAssignedRegister(NULL);
         _registerFile[i]->setAssignedRegister(NULL);
         }
      }
   }

TR::RegisterDependencyConditions *OMR::ARM64::Machine::createDepCondForLiveGPRs(TR::list<TR::Register*> *spilledRegisterList)
   {
   int32_t i, c=0;
   // Calculate number of register dependencies required. This step is not really necessary, but
   // it is space conscious
   //
   TR::Compilation *comp = self()->cg()->comp();
   for (i = TR::RealRegister::FirstGPR; i < TR::RealRegister::NumRegisters - 1; i++)
      {
      TR::RealRegister *realReg = self()->getRealRegister(static_cast<TR::RealRegister::RegNum>(i));

      TR_ASSERT(realReg->getState() == TR::RealRegister::Assigned ||
              realReg->getState() == TR::RealRegister::Free ||
              realReg->getState() == TR::RealRegister::Locked,
              "cannot handle realReg state %d, (block state is %d)\n",realReg->getState(),TR::RealRegister::Blocked);

      if (realReg->getState() == TR::RealRegister::Assigned)
         c++;
      }

   c += spilledRegisterList ? spilledRegisterList->size() : 0;

   TR::RegisterDependencyConditions *deps = NULL;

   if (c)
      {
      deps = new (self()->cg()->trHeapMemory()) TR::RegisterDependencyConditions(0, c, self()->cg()->trMemory());
      for (i = TR::RealRegister::FirstGPR; i < TR::RealRegister::NumRegisters - 1; i++)
         {
         TR::RealRegister *realReg = self()->getRealRegister(static_cast<TR::RealRegister::RegNum>(i));
         if (realReg->getState() == TR::RealRegister::Assigned)
            {
            TR::Register *virtReg = realReg->getAssignedRegister();
            TR_ASSERT(!spilledRegisterList || !(std::find(spilledRegisterList->begin(), spilledRegisterList->end(), virtReg) != spilledRegisterList->end())
            ,"a register should not be in both an assigned state and in the spilled list\n");

            deps->addPostCondition(virtReg, realReg->getRegisterNumber());

            // This method is called by ARM64OutOfLineCodeSection::assignRegister only.
            // Inside the caller, the register dependency condition this method returns
            // is set to the entry label instruction of the cold path, and bookkeeping of
            // register use count is done. During bookkeeping, only total/out of line use count of
            // registers are increased, so we need to manually increase future use count here.
            virtReg->incFutureUseCount();
            }
         }
      }

   if (spilledRegisterList)
      {
      for (auto li = spilledRegisterList->begin(); li != spilledRegisterList->end(); ++li)
         {
         TR::Register* virtReg = *li;
         deps->addPostCondition(virtReg, TR::RealRegister::SpilledReg);

         // we need to manually increase future use count here too.
         virtReg->incFutureUseCount();
         }
      }

   return deps;
   }

/**
 * @brief Decrease future use count of the register and unlatch it if necessary
 *
 * @param currentInstruction     : instruction
 * @param virtualRegister        : virtual register
 *
 * @details
 * This method decrements the future use count of the given virtual register. If register
 * assignment is currently stepping through an out of line code section it also decrements
 * the out of line use count. If the future use count has reached 0, or if register assignment
 * is currently stepping through the 'hot path' of a corresponding out of line code section
 * and the future use count is equal to the out of line use count (indicating that there are
 * no further uses of this virtual register in any non-OOL path) it will unlatch the register.
 * (If the register has any OOL uses remaining it will be restored to its previous assignment
 * elsewhere.)
 * We borrowed the code from p codegen regarding out of line use count.
 * P codegen uses the out of line use count of the register to judge if there are no more uses of the register.
 * Z codegen does it differently. It uses the start range of the instruction.
 * We cannot use the same approach with z because we would have problem when the instruction
 * uses the same virtual register multiple times (e.g. same register for source and target).
 * Thus, we rely on the out of line use count as p codegen does.
 */
void OMR::ARM64::Machine::decFutureUseCountAndUnlatch(TR::Instruction *currentInstruction, TR::Register *virtualRegister)
   {
   TR::CodeGenerator *cg = self()->cg();
   TR_Debug *debugObj = cg->getDebug();

   virtualRegister->decFutureUseCount();

   TR_ASSERT(virtualRegister->getFutureUseCount() >= 0,
            "\nRegister assignment: register [%s] futureUseCount should not be negative (for node [%s], ref count=%d) !\n",
            cg->getDebug()->getName(virtualRegister),
            cg->getDebug()->getName(currentInstruction->getNode()),
            currentInstruction->getNode()->getReferenceCount());

   if (cg->isOutOfLineColdPath())
      virtualRegister->decOutOfLineUseCount();

   TR_ASSERT(virtualRegister->getFutureUseCount() >= virtualRegister->getOutOfLineUseCount(),
            "\nRegister assignment: register [%s] Future use count (%d) less than out of line use count (%d)\n",
            cg->getDebug()->getName(virtualRegister),
            virtualRegister->getFutureUseCount(),
            virtualRegister->getOutOfLineUseCount());

   // This register should be unlatched if there are no more uses
   // or
   // if we're currently in the hot path and all remaining uses are out of line.
   //
   // If the only remaining uses are out of line, then this register should be unlatched
   // here, and when the register allocator reaches the branch to the outlined code it
   // will revive the register and proceed to allocate registers in the outlined code,
   // where presumably the future use count will finally hit 0.
   if (virtualRegister->getFutureUseCount() == 0 ||
       (self()->cg()->isOutOfLineHotPath() && virtualRegister->getFutureUseCount() == virtualRegister->getOutOfLineUseCount()))
      {
      if (virtualRegister->getFutureUseCount() != 0)
         {
         if (debugObj)
            {
            self()->cg()->traceRegisterAssignment("\nOOL: %s's remaining uses are out-of-line, unlatching\n", debugObj->getName(virtualRegister));
            }
         }
      virtualRegister->getAssignedRealRegister()->setAssignedRegister(NULL);
      virtualRegister->getAssignedRealRegister()->setState(TR::RealRegister::Unlatched);
      virtualRegister->setAssignedRegister(NULL);
      }
   }
