!strict

type PlayerData = &<
    name: string,
    score: number
>

const MAX_SCORE: number = 100

fn clamp_score(score: number): number
    if score > MAX_SCORE then
        return MAX_SCORE
    end
    return score
end

let player: PlayerData = &<
    name = "Ada",
    score = 10,
    level = 2
>

let scores: &<string, number> = &<
    high = player.score
>

scores<"low"> = clamp_score(5)
print(player.name)
print(scores<"low">)
