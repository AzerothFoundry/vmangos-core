/*
 * Copyright (C) 2005-2013 MaNGOS <http://getmangos.com/>
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

#include "packet_builder.h"
#include "MoveSpline.h"
#include "WorldPacket.h"
#include "World.h"

namespace Movement
{
inline void operator << (ByteBuffer& b, Vector3 const& v)
{
    b << v.x << v.y << v.z;
}

inline void operator >> (ByteBuffer& b, Vector3& v)
{
    b >> v.x >> v.y >> v.z;
}

enum MonsterMoveType
{
    MonsterMoveNormal       = 0,
    MonsterMoveStop         = 1,
    MonsterMoveFacingSpot   = 2,
    MonsterMoveFacingTarget = 3,
    MonsterMoveFacingAngle  = 4
};

void PacketBuilder::WriteCommonMonsterMovePart(MoveSpline const& move_spline, WorldPacket& data)
{
    MoveSplineFlag splineflags = move_spline.splineflags;

    // Continuations start where the server is now, not at the old path origin.
    data << (move_spline.splineflags & MoveSplineFlag::Mask_CatmullRom
        ? move_spline.spline.getPoint(move_spline.spline.first())
        : Vector3(move_spline.ComputePosition()));
    data << move_spline.GetId();

    switch (splineflags & MoveSplineFlag::Mask_Final_Facing)
    {
        case MoveSplineFlag::Final_Target:
            data << uint8(MonsterMoveFacingTarget);
            data << move_spline.facing.target;
            break;
        case MoveSplineFlag::Final_Angle:
            data << uint8(MonsterMoveFacingAngle);
            data << move_spline.facing.angle;
            break;
        case MoveSplineFlag::Final_Point:
            data << uint8(MonsterMoveFacingSpot);
            data << move_spline.facing.f.x << move_spline.facing.f.y << move_spline.facing.f.z;
            break;
        default:
            data << uint8(MonsterMoveNormal);
            break;
    }

    // add fake Enter_Cycle flag - needed for client-side cyclic movement (client will erase first spline vertex after first cycle done)
    splineflags.enter_cycle = move_spline.isCyclic();
    data << uint32(splineflags & ~MoveSplineFlag::Mask_No_Monster_Move);
    data << move_spline.Duration();
}

namespace {
Vector3 LinearOffset(Vector3 const& destination, Vector3 const& point)
{
    Vector3 offset = destination - point;
    // [-ZERO] Preserve the native near-zero workaround, including during bounds checks.
    if (fabs(offset.x) < 0.25 && fabs(offset.y) < 0.25 && fabs(offset.z) < 0.25)
        offset.z += offset.z < 0 ? 0.51f : 0.26f;
    return offset;
}
bool PackedComponentFits(float value, int minimum, int maximum)
{
    // appendPackXYZ rounds to quarter yards BEFORE masking. Test that actual integer,
    // not a float magnitude approximation: negative and positive bounds differ.
    double quantized = std::round(double(value) * 4.0);
    return std::isfinite(quantized) && quantized >= minimum && quantized <= maximum;
}
bool PackedOffsetFits(Vector3 const& offset)
{
    return PackedComponentFits(offset.x, -1024, 1023) &&
           PackedComponentFits(offset.y, -1024, 1023) &&
           PackedComponentFits(offset.z, -512, 511);
}
}
int32 WriteLinearPath(Spline<int32> const& spline, ByteBuffer& data, uint32 start, uint32 maxPoints)
{
    uint32 end = spline.last();
    uint32 budget = std::max(uint32(1), maxPoints);
    uint32 limit = start + std::min(end - start, budget - 1);
    uint32 last = start; // Header origin + one float endpoint always forms a legal two-point move.
    for (uint32 candidate = start + 1; candidate <= limit; ++candidate)
    {
        bool fits = true;
        for (uint32 i = start; i < candidate; ++i)
            if (!PackedOffsetFits(LinearOffset(spline.getPoint(candidate), spline.getPoint(i))))
            { fits = false; break; }
        // A later endpoint can fit after an earlier one failed (a returning curve).
        if (fits) last = candidate;
    }
    Vector3 const& destination = spline.getPoint(last);
    data << (last - start + 1);
    data << destination;
    for (uint32 i = start; i < last; ++i)
    {
        Vector3 offset = LinearOffset(destination, spline.getPoint(i));
        data.appendPackXYZ(offset.x, offset.y, offset.z);
    }
    return last < end ? int32(last) : -1;
}

void WriteCatmullRomPath(Spline<int32> const& spline, ByteBuffer& data)
{
    uint32 count = spline.getPointCount() - 3;
    data << count;
    data.append<Vector3>(&spline.getPoint(2), count);
}

void WriteCatmullRomCyclicPath(Spline<int32> const& spline, ByteBuffer& data)
{
    uint32 count = spline.getPointCount() - 3;
    data << uint32(count + 1);
    data << spline.getPoint(1); // fake point, client will erase it from the spline after first cycle done
    data.append<Vector3>(&spline.getPoint(1), count);
}

int PacketBuilder::WriteMonsterMove(MoveSpline const& move_spline, WorldPacket& data, int firstPoint)
{
    return WriteMonsterMove(move_spline, data, firstPoint, sWorld.getConfig(CONFIG_UINT32_MAX_POINTS_PER_MVT_PACKET));
}

int PacketBuilder::WriteMonsterMove(MoveSpline const& move_spline, WorldPacket& data, int firstPoint, unsigned maxPoints)
{
    WriteCommonMonsterMovePart(move_spline, data);
    size_t durationPos = data.wpos() - 4;

    Spline<int32> const& spline = move_spline.spline;
    MoveSplineFlag splineflags = move_spline.splineflags;
    if (splineflags & MoveSplineFlag::Mask_CatmullRom)
    {
        if (splineflags.cyclic)
            WriteCatmullRomCyclicPath(spline, data);
        else
            WriteCatmullRomPath(spline, data);
        return -1;
    }
    else
    {
        // Include every unvisited corner, including the endpoint of the current segment.
        firstPoint = std::max(firstPoint, move_spline._currentSplineIdx() + 1);
        int32 lastNode = WriteLinearPath(spline, data, firstPoint, maxPoints);
        int32 arrival = spline.length(lastNode >= 0 ? lastNode : spline.last());
        data.put<uint32>(durationPos, uint32(std::max(1, arrival - move_spline.timePassed())));
        return lastNode;
    }
}

bool PacketBuilder::WriteContinuation(MoveSpline& move, WorldPacket& data)
{
    return WriteContinuation(move, data, sWorld.getConfig(CONFIG_UINT32_MAX_POINTS_PER_MVT_PACKET));
}

bool PacketBuilder::WriteContinuation(MoveSpline& move, WorldPacket& data, unsigned maxPoints)
{
    if (!move.NeedsPacketContinuation()) return false;
    move.MarkPacketContinuationChecked();
    int previous = move.getLastPointSent();
    int next = WriteMonsterMove(move, data, move._currentSplineIdx() + 1, maxPoints);
    // A clipped prefix can still end at the same point. Only advertise actual progress.
    if (next >= 0 && next <= previous) return false;
    move.setLastPointSent(next);
    return true;
}

void PacketBuilder::WriteCreate(MoveSpline const& move_spline, ByteBuffer& data)
{
    // WriteClientStatus(mov,data);
    // data.append<float>(&mov.m_float_values[SpeedWalk], SpeedMaxCount);
    if (move_spline.Initialized())
    {
        MoveSplineFlag splineFlags = move_spline.splineflags;

        data << splineFlags.raw();

        if (splineFlags.final_angle)
            data << move_spline.facing.angle;
        else if (splineFlags.final_target)
            data << move_spline.facing.target;
        else if (splineFlags.final_point)
            data << move_spline.facing.f.x << move_spline.facing.f.y << move_spline.facing.f.z;

        data << move_spline.timePassed();
        data << move_spline.Duration();
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_7_1
        data << move_spline.GetId();
#endif

        uint32 nodes = move_spline.getPath().size();
        data << nodes;

        if (!move_spline.isCyclic())
        {
            for (uint32 i = 0; i < move_spline.getPath().size(); ++i)
            {
                float offset = 0.0f;

                // 1.4 client crashes with assert when it gets a zero offset.
                if (i > 0 && (move_spline.getPath()[i - 1] - move_spline.getPath()[i]).isZero())
                {
                    if (i & 1)
                        offset = 0.01f;
                    else
                        offset = 0.02f;
                }
                
                data << float(move_spline.getPath()[i].x);
                data << float(move_spline.getPath()[i].y);
                data << float(move_spline.getPath()[i].z + offset);
            }
        }
        else
            data.append<Vector3>(&move_spline.getPath()[0], nodes);

        data << (move_spline.isCyclic() ? Vector3::zero() : move_spline.FinalDestination());
    }
}
}
